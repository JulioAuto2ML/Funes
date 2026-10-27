#!/usr/bin/env python3
"""
publish_voc_post.py — Posts approved VoC articles to LinkedIn.

Usage:
    python3 publish_voc_post.py --dir PATH [--dry-run] [--env-dir PATH]

Scans data/voc-pipeline/articles/ for directories that have a final.md
but no matching record in data/voc-pipeline/published/. For each, extracts
the LinkedIn post text from a <!-- linkedin: ... --> block in final.md,
and posts it.

The post text is authored by you during review, not extracted or condensed
by this script. If final.md has no linkedin block, the article is skipped
with a warning.

LinkedIn credentials are read from <env-dir>/.env (defaults to the
publishing/ directory in the Funes repo, which is where post_tweet.py
reads them from too).

Exit codes:
    0  posted at least one (or, with --dry-run, would have)
    1  usage/input error
    3  nothing to publish (no approved articles without a publish record)
"""

import json
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

SCRIPT_DIR = Path(__file__).parent.resolve()
REPO_ROOT = SCRIPT_DIR.parent

sys.path.insert(0, str(REPO_ROOT / "publishing"))
from post_tweet import load_env, get_valid_token, post_to_linkedin  # noqa: E402


LINKEDIN_BLOCK_RE = re.compile(
    r"<!--\s*linkedin:\s*\n(.*?)\n\s*-->",
    re.DOTALL,
)


def extract_linkedin_text(final_md: Path) -> str | None:
    """Extract the <!-- linkedin: ... --> block from final.md."""
    content = final_md.read_text(encoding="utf-8")
    match = LINKEDIN_BLOCK_RE.search(content)
    if not match:
        return None
    return match.group(1).strip()


def find_unpublished(pipeline_dir: Path) -> list[tuple[str, Path]]:
    """Return (slug, final.md path) for articles ready to publish."""
    articles_dir = pipeline_dir / "articles"
    published_dir = pipeline_dir / "published"
    if not articles_dir.is_dir():
        return []

    results = []
    for article_dir in sorted(articles_dir.iterdir()):
        if not article_dir.is_dir():
            continue
        slug = article_dir.name
        final = article_dir / "final.md"
        record = published_dir / f"{slug}.json"
        if final.exists() and not record.exists():
            results.append((slug, final))
    return results


def publish_one(slug: str, final_path: Path, pipeline_dir: Path,
                token: str, dry_run: bool) -> bool:
    """Publish one article. Returns True if posted (or would have)."""
    text = extract_linkedin_text(final_path)
    if text is None:
        print(f"  SKIP {slug}: no <!-- linkedin: ... --> block in final.md")
        return False

    if len(text) > 3000:
        print(f"  SKIP {slug}: linkedin block is {len(text)} chars (max ~2900)")
        return False

    print(f"  {slug}:")
    print(f"    {text[:120]}{'...' if len(text) > 120 else ''}")
    print(f"    ({len(text)} chars)")

    if dry_run:
        print("    [DRY RUN] not posted.")
        return True

    post_id = post_to_linkedin(text, token)

    published_dir = pipeline_dir / "published"
    published_dir.mkdir(parents=True, exist_ok=True)
    record = {
        "slug": slug,
        "published_at": datetime.now(timezone.utc).isoformat(),
        "platform": "linkedin",
        "post_id": post_id,
        "post_text": text,
    }
    (published_dir / f"{slug}.json").write_text(
        json.dumps(record, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"    Posted! ID: {post_id}")
    return True


def main():
    args = sys.argv[1:]

    def take(flag):
        if flag not in args:
            return None
        i = args.index(flag)
        if i + 1 >= len(args):
            print(f"ERROR: {flag} needs a value")
            sys.exit(1)
        value = args[i + 1]
        del args[i:i + 2]
        return value

    pipeline_dir_str = take("--dir")
    env_dir_str = take("--env-dir")
    dry_run = "--dry-run" in args
    if "--dry-run" in args:
        args.remove("--dry-run")

    if not pipeline_dir_str:
        print("Usage: python3 publish_voc_post.py --dir PATH [--dry-run] [--env-dir PATH]")
        return 1

    pipeline_dir = Path(pipeline_dir_str)
    if not pipeline_dir.is_dir():
        print(f"ERROR: {pipeline_dir} is not a directory")
        return 1

    unpublished = find_unpublished(pipeline_dir)
    if not unpublished:
        print("Nothing to publish — no approved articles without a publish record.")
        return 3

    print(f"Found {len(unpublished)} article(s) to publish:\n")

    env_dir = Path(env_dir_str) if env_dir_str else REPO_ROOT / "publishing"
    token = None
    if not dry_run:
        creds = load_env(env_dir / ".env")
        token = get_valid_token(creds)

    posted = 0
    for slug, final_path in unpublished:
        if publish_one(slug, final_path, pipeline_dir, token, dry_run):
            posted += 1

    print(f"\n{'Would post' if dry_run else 'Posted'} {posted}/{len(unpublished)} article(s).")
    return 0 if posted > 0 else 3


if __name__ == "__main__":
    sys.exit(main())
