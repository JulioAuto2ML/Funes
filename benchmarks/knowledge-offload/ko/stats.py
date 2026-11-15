"""Statistics from "The paper -> Statistics": bootstrap CIs over questions,
paired comparison, run-to-run spread, McNemar. Stdlib only."""
import math
import random
import statistics


def bootstrap_ci(values: list[float], resamples: int = 10_000, seed: int = 0,
                 alpha: float = 0.05) -> tuple[float, float]:
    n = len(values)
    if n == 0:
        return (float("nan"), float("nan"))
    rng = random.Random(seed)
    means = sorted(sum(values[rng.randrange(n)] for _ in range(n)) / n for _ in range(resamples))
    return (means[int(alpha / 2 * resamples)], means[int((1 - alpha / 2) * resamples) - 1])


def paired_bootstrap(a: list[float], b: list[float], resamples: int = 10_000, seed: int = 0):
    """Difference mean(a)-mean(b) over the same questions, with its 95% CI."""
    assert len(a) == len(b)
    d = [x - y for x, y in zip(a, b)]
    return sum(d) / len(d) if d else float("nan"), bootstrap_ci(d, resamples, seed)


def mcnemar(a: list[bool], b: list[bool]) -> dict:
    """Exact two-sided McNemar on paired correctness: only discordant pairs count."""
    only_a = sum(1 for x, y in zip(a, b) if x and not y)
    only_b = sum(1 for x, y in zip(a, b) if y and not x)
    n = only_a + only_b
    if n == 0:
        return {"only_a": 0, "only_b": 0, "p": 1.0}
    k = min(only_a, only_b)
    p = min(1.0, 2 * sum(math.comb(n, i) for i in range(k + 1)) / 2 ** n)
    return {"only_a": only_a, "only_b": only_b, "p": p}


def spread(per_repeat: list[float]) -> dict:
    """Run-to-run noise of one cell: the spread across its repeats."""
    return {"n": len(per_repeat), "mean": statistics.fmean(per_repeat),
            "sd": statistics.stdev(per_repeat) if len(per_repeat) > 1 else 0.0,
            "range": max(per_repeat) - min(per_repeat)}
