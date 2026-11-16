"""The eight conditions. One function per condition: item -> Answer.

What differs between them is only what the model is given and may do; the
system prompt, decoding settings, k and the step cap are shared constants, so a
change to any of them is a new row in the notebook, not a quiet edit here.
"""
from dataclasses import dataclass, field

from .llm import Client, Reply, Usage
from .popqa import Item
from .store import Fact, FactStore
from . import tools as T

CONDITIONS = ["closed", "oracle", "store", "tool", "tool+contract", "funes", "web", "web+cache"]

K = 5                # facts handed to / returned by the store
MAX_STEPS = 4        # tool rounds before tools are withheld and an answer forced
MAX_NUDGES = 2       # contract: how often a text answer is refused before giving up
MAX_TOKENS = 128

SYSTEM = ("You answer factual questions. Reply with only the answer: a short phrase, "
          "with no explanation and no full sentence.")
SYSTEM_TOOL = SYSTEM + " Use the tools you are given when they help."
# Prompt variants (a new row, never an edit of the old one). On the 9B the default
# line above made the model answer from memory and skip the tools (6 lookups in
# 1000 questions, 1 call/answer and no search in `web`): "reply with only the
# answer" outweighed "use the tools". `search_first` says when to use them.
VARIANTS = ["default", "search_first", "search_first2", "abstain_ok"]
_SF = {
    "search_first": (
        "You answer factual questions about specific people, places and works. Your memory of them "
        "is unreliable. When a question names a specific entity, you MUST first call {tool}, then "
        "answer from what it returns; never answer such a question from memory. "
        "The final answer is still only a short phrase, with no explanation and no full sentence."),
    "search_first2": (
        "Task: answer the question with a short phrase. Rule: before you write any answer, make a "
        "{tool} call for the entity the question names. An answer with no {tool} call before it is "
        "wrong. After the call, reply with only the answer: a short phrase, nothing else."),
}
# `abstain_ok` (the `ablated` arm): a way out that is not a guess. Tool conditions keep the
# search_first instruction, so an abstention has to come after a lookup that found nothing.
ABSTAIN_PLAIN = (" If you do not know the answer and the facts given, if any, do not contain it, "
                 "reply exactly: I don't know.")
ABSTAIN_TOOL = (" If the lookup returns nothing that answers the question and you do not know the "
                "answer, reply exactly: I don't know.")
TOOL_CONDITIONS = ("tool", "tool+contract", "web", "web+cache")
VARIANT_APPLIES = {"default": (), "search_first": TOOL_CONDITIONS, "search_first2": TOOL_CONDITIONS,
                   "abstain_ok": ("closed", "store", "tool", "tool+contract")}
ABLATED_CONDITIONS = ("closed", "store", "tool", "tool+contract")


def variant_for(condition: str, variant: str) -> str:
    """The variant a condition actually runs under; one it does not apply to runs `default`."""
    return variant if condition in VARIANT_APPLIES[variant] else "default"


NUDGE = "You have not used {tool} yet. Call it before you answer."


class ConditionUnavailable(RuntimeError):
    """A condition that cannot run here (no key, no instance). Never faked."""


@dataclass
class Answer:
    text: str = ""
    usage: Usage = field(default_factory=Usage)
    tools: list = field(default_factory=list)     # names, in call order
    steps: int = 0
    nudges: int = 0
    contract_met: bool | None = None
    retrieval_hit: bool | None = None
    tainted: bool = False
    error: str | None = None
    extra: dict = field(default_factory=dict)
    payloads: list = field(default_factory=list)    # raw web payloads; stored beside, not in, the result


def system_for(ctx, tool: str) -> str:
    """The system prompt of a tool-using condition; `tool` names what to call first."""
    v = getattr(ctx, "variant", "default")
    if v == "abstain_ok":
        return _SF["search_first"].format(tool=tool) + ABSTAIN_TOOL
    return SYSTEM_TOOL if v == "default" else _SF[v].format(tool=tool)


@dataclass
class Context:
    client: Client | None = None
    store: FactStore | None = None
    seed: int = 0
    web: T.Web | None = None
    notes: T.Notes | None = None
    funes: object = None            # ko.funes.FunesInstance
    passno: int = 1
    variant: str = "default"
    ablated: bool = False           # the question's own triple is withheld from retrieval


def _gold(item: Item) -> Fact:
    return Fact(item.subj, item.prop, item.obj)


def _norm_hit(facts, item: Item) -> bool:
    from .scoring import normalize
    al = {normalize(a) for a in item.aliases}
    return any(normalize(f.subj) == normalize(item.subj) and normalize(f.prop) == normalize(item.prop)
               and normalize(f.obj) in al for f in facts)


def _exclude(ctx: Context, item: Item):
    return _gold(item) if ctx.ablated else None


def _plain(ctx: Context, item: Item, user: str) -> Answer:
    system = SYSTEM + (ABSTAIN_PLAIN if ctx.variant == "abstain_ok" else "")
    r = ctx.client.chat([{"role": "system", "content": system},
                         {"role": "user", "content": user}], seed=ctx.seed, max_tokens=MAX_TOKENS)
    return Answer(text=r.text, usage=r.usage, steps=1)


def closed(ctx, item):
    return _plain(ctx, item, item.question)


def oracle(ctx, item):
    return _plain(ctx, item, f"Facts:\n- {_gold(item).text()}\n\nQuestion: {item.question}")


def store(ctx, item):
    facts = ctx.store.search(item.question, k=K, exclude=_exclude(ctx, item))
    listing = "\n".join(f"- {f.text()}" for f in facts) or "- (nothing retrieved)"
    a = _plain(ctx, item, f"Facts:\n{listing}\n\nQuestion: {item.question}")
    a.retrieval_hit = _norm_hit(facts, item)
    return a


def tool_loop(ctx: Context, item_question: str, schemas, executors, require=None,
              system=SYSTEM_TOOL) -> Answer:
    """The agent loop in miniature, with two of Funes's layers: the last step
    withholds tools (a forced written answer), and `require` is the completion
    contract — a text answer is refused until that tool has succeeded."""
    msgs = [{"role": "system", "content": system}, {"role": "user", "content": item_question}]
    ans = Answer(contract_met=None if not require else False)
    for step in range(MAX_STEPS + 1):
        last = step == MAX_STEPS
        r: Reply = ctx.client.chat(msgs, tools=None if last else schemas,
                                   seed=ctx.seed, max_tokens=MAX_TOKENS)
        ans.usage.add(r.usage)
        ans.steps += 1
        if r.tool_calls and not last:
            msgs.append({**r.raw_message, "content": r.raw_message.get("content") or ""})
            for call in r.tool_calls:
                ans.tools.append(call["name"])
                fn = executors.get(call["name"])
                try:
                    out = fn(call["arguments"]) if fn else f"error: no tool {call['name']}"
                    ok = fn is not None
                except T.SearchBlocked:
                    raise                                       # the backend is rate-limiting: stop the run
                except Exception as e:                          # tool failure is data
                    out, ok = f"error: {e}", False
                if ok and call["name"] == require:
                    ans.contract_met = True
                msgs.append({"role": "tool", "tool_call_id": call["id"], "content": out})
            continue
        if require and not ans.contract_met and ans.nudges < MAX_NUDGES and not last:
            ans.nudges += 1
            msgs.append({"role": "assistant", "content": r.text})
            msgs.append({"role": "user", "content": NUDGE.format(tool=require)})
            continue
        ans.text = r.text
        return ans
    return ans


def tool(ctx, item):
    return tool_loop(ctx, item.question, [T.LOOKUP],
                     {"lookup": lambda a: T.run_lookup(ctx.store, a, _exclude(ctx, item))},
                     system=system_for(ctx, "the lookup tool"))


def tool_contract(ctx, item):
    return tool_loop(ctx, item.question, [T.LOOKUP],
                     {"lookup": lambda a: T.run_lookup(ctx.store, a, _exclude(ctx, item))}, require="lookup",
                     system=system_for(ctx, "the lookup tool"))


def _web_executors(ctx):
    return {"web_search": lambda a: ctx.web.search(str(a.get("query", ""))),
            "web_fetch": lambda a: ctx.web.fetch(str(a.get("url", "")))}


def web(ctx, item):
    if ctx.web is None or not ctx.web.available:
        raise ConditionUnavailable("no search backend (FUNES_SEARCH_BACKEND=parallel, or a Tavily key)")
    n0, p0 = len(ctx.web.calls), len(ctx.web.payloads)
    a = tool_loop(ctx, item.question, [T.WEB_SEARCH, T.WEB_FETCH], _web_executors(ctx),
                  system=system_for(ctx, "web_search"))
    a.extra["search_backends"] = ctx.web.calls[n0:]
    a.payloads = ctx.web.payloads[p0:]
    del ctx.web.payloads[p0:]          # handed to the answer; the run keeps nothing
    a.tainted = any(t.startswith("web_") for t in a.tools)   # outside content was read
    return a


def web_cache(ctx, item):
    """`web`, plus recall/remember over this run's own notes. Run as two passes
    over the same questions (runner); pass 2 is the number H6 is about."""
    if ctx.web is None or not ctx.web.available:
        raise ConditionUnavailable("no search backend (FUNES_SEARCH_BACKEND=parallel, or a Tavily key)")
    ex = _web_executors(ctx)
    ex["recall"] = lambda a: ctx.notes.recall(str(a.get("query", "")))
    ex["remember"] = lambda a: ctx.notes.remember(str(a.get("fact", "")))
    system = (system_for(ctx, "recall, and web_search if it has nothing") + " First call recall to check what you already noted. If it does not "
              "answer the question, search the web, then call remember with one short fact "
              "that includes its source URL.")
    before, n0, p0 = ctx.notes.served, len(ctx.web.calls), len(ctx.web.payloads)
    a = tool_loop(ctx, item.question, [T.RECALL, T.WEB_SEARCH, T.WEB_FETCH, T.REMEMBER], ex,
                  system=system)
    # A note came from a web page: an answer built on one is as tainted as the search.
    a.extra["search_backends"] = ctx.web.calls[n0:]
    a.payloads = ctx.web.payloads[p0:]
    del ctx.web.payloads[p0:]          # handed to the answer; the run keeps nothing
    a.tainted = any(t.startswith("web_") for t in a.tools) or ctx.notes.served > before
    a.extra["pass"] = ctx.passno
    return a


def funes(ctx, item):
    if ctx.funes is None:
        raise ConditionUnavailable("no scratch Funes instance (pass --funes-bin or run from the repo root)")
    return ctx.funes.ask(item.question)


REGISTRY = {"closed": closed, "oracle": oracle, "store": store, "tool": tool,
            "tool+contract": tool_contract, "funes": funes, "web": web, "web+cache": web_cache}
