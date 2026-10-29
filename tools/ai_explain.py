#!/usr/bin/env python3
"""Optional AI layer: asks a language model to explain the measured findings.

    python3 tools/ai_explain.py results/results.csv [trace.csv ...]

1. Runs the deterministic report (trace_report.py). That part is always printed.
2. If ANTHROPIC_API_KEY is set, sends the report to the Claude API and prints
   the reply under a heading that marks it as suggestions, not measurements.
   Without a key it saves the prompt to ai_prompt.txt and stops.

The AI never sees the source code or raw timings beyond the report, and nothing
it says is fed back into the numbers.
"""
import json
import os
import sys
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_report import build_report  # noqa: E402

API_URL = "https://api.anthropic.com/v1/messages"
MODEL = os.environ.get("PARAFORGE_MODEL", "claude-sonnet-5-5")

PROMPT = """You are reviewing benchmark results of a C++ task runtime with three
schedulers (static split, one shared queue, work stealing) running matrix
multiply, BFS, chess alpha-beta search, a swarm simulation and a small
functional language interpreter.

Below is a rule-based report. Every number in it was measured.

{report}

For each finding: give the most likely cause in one or two sentences, say how
confident you are, and propose one concrete experiment that would confirm or
reject it. Do not invent numbers. If a finding looks like noise, say so."""


def ask_model(prompt, api_key):
    body = json.dumps({
        "model": MODEL,
        "max_tokens": 1500,
        "messages": [{"role": "user", "content": prompt}],
    }).encode()
    req = urllib.request.Request(API_URL, data=body, headers={
        "x-api-key": api_key,
        "anthropic-version": "2023-06-01",
        "content-type": "application/json",
    })
    with urllib.request.urlopen(req, timeout=120) as resp:
        data = json.load(resp)
    return "".join(block.get("text", "") for block in data.get("content", []))


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: ai_explain.py results.csv [trace.csv ...]")
    report = build_report(sys.argv[1], sys.argv[2:])
    print(report)
    print()

    prompt = PROMPT.format(report=report)
    api_key = os.environ.get("ANTHROPIC_API_KEY")
    if not api_key:
        with open("ai_prompt.txt", "w") as f:
            f.write(prompt)
        print("No ANTHROPIC_API_KEY set: the measured report above is complete.")
        print("The prompt was saved to ai_prompt.txt if you want to paste it into a chat.")
        return

    print("=" * 70)
    print("AI SUGGESTIONS - not measured, check each one with an experiment")
    print("=" * 70)
    try:
        print(ask_model(prompt, api_key))
    except urllib.error.URLError as e:
        print(f"API call failed ({e}); the measured report above is still valid.")


if __name__ == "__main__":
    main()
