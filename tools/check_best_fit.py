"""Compare the executable with an independent, meter-by-meter Best Fit model."""
import argparse
from pathlib import Path
import random
import subprocess


def expected_replies(commands):
    cells = []
    pipes = [None]
    answers = []
    for command in commands:
        if command < 0:
            pid = -command
            start, length = pipes[pid]
            assert cells[start:start + length] == [pid] * length
            cells[start:start + length] = [0] * length
            pipes[pid] = None
            answers.append(str(start))
            continue
        gaps = []
        i = 0
        while i < len(cells):
            if cells[i]:
                i += 1
                continue
            start = i
            while i < len(cells) and cells[i] == 0:
                i += 1
            if i - start >= command:
                gaps.append((i - start, start))
        digs = 0
        if gaps:
            _, start = min(gaps)
        else:
            start = len(cells)
            while start > 0 and cells[start - 1] == 0:
                start -= 1
            missing = command - (len(cells) - start)
            digs = (missing + 19) // 20
            cells.extend([0] * (digs * 20))
        assert start + command <= len(cells)
        assert cells[start:start + command] == [0] * command
        cells[start:start + command] = [len(pipes)] * command
        pipes.append((start, command))
        answers.append(f"{digs} {start}")
    return answers


def check(solution, commands, expected):
    payload = "\n".join(map(str, commands + [0])) + "\n"
    result = subprocess.run([str(solution)], input=payload, text=True,
                            capture_output=True, timeout=15, check=True)
    assert not result.stderr, result.stderr
    actual = result.stdout.splitlines()
    assert len(actual) == len(expected), (len(actual), len(expected))
    for step, (got, want) in enumerate(zip(actual, expected), 1):
        assert got == want, f"Command {step}: expected {want!r}, got {got!r}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("solution", type=Path)
    solution = parser.parse_args().solution.resolve(strict=True)
    scenarios = [
        [1, 4, 15, -2, 3, -1],               # example
        [19, 2, 40],                         # tail reuse, rounded digging
        [5, 5, 5, -1, -3, -2, 20],          # merge both neighbors and tail
        [10, 1, 10, 1, -1, -3, 10, 10],    # equal gaps: leftmost first
        [4, 10, 6, -1, -3, 3, 4, 3],       # fragmentation under Best Fit
    ]
    for seed in range(20):
        rng = random.Random(seed)
        commands, live, pid = [], [], 0
        for _ in range(1000):
            if not live or rng.random() < 0.58:
                pid += 1
                commands.append(rng.randint(1, 40))
                live.append(pid)
            else:
                index = rng.randrange(len(live))
                commands.append(-live.pop(index))
        scenarios.append(commands)
    for commands in scenarios:
        check(solution, commands, expected_replies(commands))
    # Full command limit and maximum pipe length; expected trench is 8,000,000 m.
    count = 200000
    check(solution, [40] * count, [f"2 {40 * i}" for i in range(count)])
    print(f"PASS: {len(scenarios)} scenarios against meter model; "
          f"{sum(map(len, scenarios))} commands; 200000-command maximum-length case")


if __name__ == "__main__":
    main()
