# `funes` rows: the deployed agent against the plain store

1,000 unambiguous PopQA items, IQ4_XS, one repeat each, `contains`. Differences are paired, 95% bootstrap over questions (10000 resamples, seed 0, one resample shared by all sizes). Descriptive: outside the Holm family. Errors are counted wrong (not re-run).

| size | store | funes | funes - store [95% CI] | only store right | only funes right | errors | any-tool rate | `recall` | `web_search` | taint rate | wall (s) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0.8b | 0.921 | 0.636 | -0.285 [-0.318, -0.253] | 320 | 35 | 4 | 0.281 | 92 | 179 | 0.181 | 2334 |
| 2b | 0.971 | 0.715 | -0.256 [-0.286, -0.227] | 272 | 16 | 11 | 0.372 | 301 | 78 | 0.078 | 2254 |
| 4b | 0.984 | 0.967 | -0.017 [-0.030, -0.004] | 32 | 15 | 0 | 0.508 | 501 | 6 | 0.006 | 3368 |
| 9b | 0.980 | 0.931 | -0.049 [-0.067, -0.032] | 66 | 17 | 1 | 0.068 | 62 | 7 | 0.007 | 4206 |
| 27b | 0.991 | 0.985 | -0.006 [-0.015, +0.003] | 13 | 7 | 1 | 0.020 | 15 | 6 | 0.006 | 13863 |

By popularity bucket (funes accuracy, n in brackets; buckets 1e0, 1e6, 1e7 are too small to read):

| size | 1e1 | 1e2 | 1e3 | 1e4 | 1e5 |
|---|---|---|---|---|---|
| 0.8b | 0.590 [195] | 0.544 [195] | 0.554 [195] | 0.662 [195] | 0.835 [194] |
| 2b | 0.779 [195] | 0.677 [195] | 0.579 [195] | 0.662 [195] | 0.892 [194] |
| 4b | 0.964 [195] | 0.949 [195] | 0.959 [195] | 0.985 [195] | 0.979 [194] |
| 9b | 0.969 [195] | 0.928 [195] | 0.867 [195] | 0.903 [195] | 0.985 [194] |
| 27b | 0.990 [195] | 0.985 [195] | 0.979 [195] | 0.979 [195] | 0.990 [194] |

Context-overflow errors (`request ... exceeds the available context`), counted wrong: 0.8b 4, 2b 11, 4b 0, 9b 1, 27b 1 (total 17 of 5000).
