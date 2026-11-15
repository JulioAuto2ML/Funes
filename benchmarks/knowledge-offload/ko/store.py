"""The fact store: subject-predicate-object triples in SQLite with FTS5.

E0 deliberately measures the *store* and not an embedding model: retrieval is
BM25 over the entity words of the question, plus exact (entity, relation)
lookup for the tool. Vectors enter with the `funes` condition, which goes
through Funes's own MemoryStore (vectors + FTS + links) — comparing the two is
E4's question, not E0's.
"""
import re
import sqlite3
from dataclasses import dataclass

from .scoring import normalize

_STOP = set("what who whom whose which where when is are was were the a an of in on to "
            "for by does do did that this with as at from".split())


@dataclass(frozen=True)
class Fact:
    subj: str
    prop: str
    obj: str

    def text(self) -> str:
        return f"{self.subj} | {self.prop} | {self.obj}"


def _terms(text: str) -> list[str]:
    return [t for t in normalize(text).split() if t not in _STOP and len(t) > 1]


class FactStore:
    def __init__(self, facts: list[Fact] = ()):
        self.db = sqlite3.connect(":memory:")
        self.db.execute("create table f(id integer primary key, subj, prop, obj, nsubj)")
        self.db.execute("create virtual table fts using fts5(subj, prop, "
                        "content='f', content_rowid='id', tokenize='unicode61 remove_diacritics 2')")
        self.add_many(facts)

    def __len__(self):
        return self.db.execute("select count(*) from f").fetchone()[0]

    def add_many(self, facts):
        for f in facts:
            self.add(f)

    def add(self, f: Fact) -> None:
        cur = self.db.execute("insert into f(subj, prop, obj, nsubj) values (?,?,?,?)",
                              (f.subj, f.prop, f.obj, normalize(f.subj)))
        self.db.execute("insert into fts(rowid, subj, prop) values (?,?,?)",
                        (cur.lastrowid, f.subj, f.prop))

    def search(self, query: str, k: int = 5) -> list[Fact]:
        terms = _terms(query)
        if not terms:
            return []
        match = " OR ".join('"%s"' % t.replace('"', "") for t in terms)
        rows = self.db.execute(
            "select f.subj, f.prop, f.obj from fts join f on f.id = fts.rowid "
            "where fts match ? order by bm25(fts, 5.0, 1.0) limit ?", (match, k)).fetchall()
        return [Fact(*r) for r in rows]

    def lookup(self, entity: str, relation: str = "", k: int = 5) -> list[Fact]:
        """Exact entity match (normalised), relation matched loosely; falls back
        to a fuzzy search on the entity if the exact key is absent — a lookup
        that fails on a typo would measure the typo, not the decision."""
        ent = normalize(entity)
        rows = self.db.execute("select subj, prop, obj from f where nsubj = ?", (ent,)).fetchall()
        facts = [Fact(*r) for r in rows] or self.search(entity, k=k)
        rel = normalize(relation)
        if rel:
            narrowed = [f for f in facts if rel in normalize(f.prop) or normalize(f.prop) in rel]
            facts = narrowed or facts
        return facts[:k]
