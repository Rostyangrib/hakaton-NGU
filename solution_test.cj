package pipeline

import std.collection.*
import std.unittest.*
import std.unittest.testmacro.*

func closeEnough(actual: Float64, expected: Float64): Bool {
    let delta = actual - expected
    return delta < 1.0e-10 && delta > -1.0e-10
}

func sameOutput(actual: (Int64, Int64), expected: (Int64, Int64)): Bool {
    return actual[0] == expected[0] && actual[1] == expected[1]
}

// Reference model: one independent cell per meter, no interval indexes.
class MeterOracle {
    public let cells = ArrayList<Int64>()
    private let positions = ArrayList<Int64>()
    private let lengths = ArrayList<Int64>()

    public init() {
        positions.add(-1)
        lengths.add(0)
    }

    public func add(length: Int64, digs: Int64, position: Int64): Unit {
        if (digs < 0 || position < 0) {
            throw IllegalStateException("Negative output")
        }
        for (_ in 0..(digs * 20)) {
            cells.add(0)
        }
        if (position + length > cells.size) {
            throw IllegalStateException("Out of trench")
        }
        let pid = positions.size
        for (index in position..(position + length)) {
            if (cells[index] != 0) {
                throw IllegalStateException("Collision in reference model")
            }
            cells[index] = pid
        }
        positions.add(position)
        lengths.add(length)
    }

    public func remove(pid: Int64, position: Int64): Unit {
        if (positions[pid] != position || lengths[pid] == 0) {
            throw IllegalStateException("Incorrect removal position")
        }
        for (index in position..(position + lengths[pid])) {
            if (cells[index] != pid) {
                throw IllegalStateException("Incorrect PID in reference model")
            }
            cells[index] = 0
        }
        lengths[pid] = 0
    }

    public func check(engine: Pipeline): Unit {
        if (!engine.checkIndexes() || cells.size != engine.trench) {
            throw IllegalStateException("Interval indexes disagree")
        }
        var position: Int64 = 0
        var occupied: Int64 = 0
        let gaps = engine.freeGaps()
        var gapIndex: Int64 = 0
        while (position < cells.size) {
            if (cells[position] != 0) {
                occupied += 1
                position += 1
            } else {
                let start = position
                while (position < cells.size && cells[position] == 0) {
                    position += 1
                }
                if (gapIndex >= gaps.size || gaps[gapIndex].start != start ||
                    gaps[gapIndex].length != position - start) {
                    throw IllegalStateException("Free intervals disagree with meter model")
                }
                gapIndex += 1
            }
        }
        if (gapIndex != gaps.size || occupied != engine.occupied) {
            throw IllegalStateException("Incorrect occupied/free balance")
        }
    }
}

// Scans every free interval. Used to verify the <=41-candidate optimization.
func fullScan(engine: Pipeline, length: Int64): Option<Gap> {
    engine.history.prepare()
    var best: Option<Gap> = None
    var bestScore: Float64 = 0.0
    for (gap in engine.freeGaps()) {
        if (gap.length < length) {
            continue
        }
        let score = engine.history.score(gap.length, length)
        if (let Some(old) <- best) {
            if (score < bestScore - SCORE_EPS ||
                (score <= bestScore + SCORE_EPS &&
                    (gap.length < old.length || (gap.length == old.length && gap.start < old.start)))) {
                best = Some(gap)
                bestScore = score
            }
        } else {
            best = Some(gap)
            bestScore = score
        }
    }
    return best
}

func sameGap(left: Option<Gap>, right: Option<Gap>): Bool {
    if (let Some(a) <- left) {
        if (let Some(b) <- right) {
            return a.start == b.start && a.length == b.length
        }
        return false
    }
    if (let Some(_) <- right) {
        return false
    }
    return true
}

class SeededNumbers {
    private var state: Int64

    public init(seed: Int64) {
        state = seed + 1
    }

    public func next(bound: Int64): Int64 {
        state = (state * 48271) % 2147483647
        return state % bound
    }
}

func randomScenario(seed: Int64, commands: Int64): Unit {
    let engine = Pipeline()
    let oracle = MeterOracle()
    let live = ArrayList<Int64>()
    let random = SeededNumbers(seed)
    for (step in 0..commands) {
        if (live.isEmpty() || random.next(100) < 58) {
            let length = random.next(40) + 1
            if (step % 17 == 0) {
                let expected = fullScan(engine, length)
                if (!sameGap(engine.chooseGap(length), expected) || engine.lastCandidateCount > 41) {
                    throw IllegalStateException("Optimized search disagrees with full scan")
                }
            }
            let (digs, start) = engine.addPipe(length)
            oracle.add(length, digs, start)
            live.add(engine.pipeCount)
        } else {
            let index = random.next(live.size)
            let pid = live[index]
            oracle.remove(pid, engine.removePipe(pid))
            live[index] = live[live.size - 1]
            live.remove(at: live.size - 1)
        }
        oracle.check(engine)
    }
}

@Test
class PipelineTests {
    @TestCase
    public func originalExample(): Unit {
        let p = Pipeline()
        @Expect(sameOutput(p.addPipe(1), (1, 0)), true)
        @Expect(sameOutput(p.addPipe(4), (0, 1)), true)
        @Expect(sameOutput(p.addPipe(15), (0, 5)), true)
        @Expect(p.removePipe(2), 1)
        @Expect(sameOutput(p.addPipe(3), (0, 1)), true)
        @Expect(p.removePipe(1), 0)
        @Expect(p.trench, 20)
        @Expect(p.occupied, 18)
        @Expect(p.checkIndexes(), true)
    }

    @TestCase
    public func tailExpansionAndFortyMeters(): Unit {
        let p = Pipeline()
        @Expect(sameOutput(p.addPipe(19), (1, 0)), true)
        @Expect(sameOutput(p.addPipe(2), (1, 19)), true)
        @Expect(sameOutput(p.addPipe(40), (2, 21)), true)
        @Expect(p.trench, 80)
        @Expect(p.checkIndexes(), true)
        let fresh = Pipeline()
        @Expect(sameOutput(fresh.addPipe(40), (2, 0)), true)
        @Expect(fresh.freeGaps().size, 0)
    }

    @TestCase
    public func exactFitAndPidAfterRemoval(): Unit {
        let p = Pipeline()
        p.addPipe(10)
        p.addPipe(10)
        p.removePipe(1)
        @Expect(sameOutput(p.addPipe(10), (0, 0)), true)
        @Expect(p.pipeCount, 3)
        @Expect(p.pipeLength(1), 0)
        @Expect(p.pipeLength(3), 10)
        @Expect(p.freeGaps().size, 0)
        @Expect(p.checkIndexes(), true)
    }

    @TestCase
    public func mergeBothNeighborsAndTail(): Unit {
        let p = Pipeline()
        p.addPipe(5)
        p.addPipe(5)
        p.addPipe(5)
        p.removePipe(1)
        p.removePipe(3)
        @Expect(p.checkIndexes(), true)
        p.removePipe(2)
        let gaps = p.freeGaps()
        @Expect(gaps.size, 1)
        @Expect(gaps[0].start, 0)
        @Expect(gaps[0].length, 20)
        @Expect(p.history.seen, 3)
        @Expect(p.history.counts[5], 3)
        @Expect(p.occupied, 0)
        @Expect(p.trench, 20)
        @Expect(p.checkIndexes(), true)
    }

    @TestCase
    public func warmupAndRampBoundaries(): Unit {
        let h = ArrivalHistory()
        for (_ in 0..199) {
            h.observe(2)
        }
        h.prepare() // choosing pipe 200
        @Expect(h.influence, 0.0)
        h.observe(2)
        h.prepare() // choosing pipe 201
        @Expect(closeEnough(h.influence, 0.005), true)
        for (_ in 0..199) {
            h.observe(2)
        }
        h.prepare() // choosing pipe 400
        @Expect(h.influence, 1.0)
    }

    @TestCase
    public func slidingWindowForgetsOldLengths(): Unit {
        let h = ArrivalHistory()
        for (_ in 0..WINDOW_SIZE) {
            h.observe(1)
        }
        for (_ in 0..WINDOW_SIZE) {
            h.observe(40)
        }
        @Expect(h.seen, 2 * WINDOW_SIZE)
        @Expect(h.size, WINDOW_SIZE)
        @Expect(h.counts[1], 0)
        @Expect(h.counts[40], WINDOW_SIZE)
        h.prepare()
        var mass: Float64 = 0.0
        for (length in 1..41) {
            @Expect(h.probabilities[length] > 0.0, true)
            mass += h.probabilities[length]
        }
        @Expect(closeEnough(mass, 1.0), true)
        @Expect(h.penalty(0), 0.0)
        @Expect(h.penalty(40), 0.0)
        @Expect(h.penalty(10_000), 0.0)
    }

    @TestCase
    public func scoringMatchesFormula(): Unit {
        let h = ArrivalHistory()
        for (_ in 0..900) {
            h.observe(1)
        }
        for (_ in 0..100) {
            h.observe(40)
        }
        h.prepare()
        let denominator = 1001.0
        var mean: Float64 = 0.0
        let raw = Array<Float64>(41, repeat: 0.0)
        for (length in 1..41) {
            raw[length] = (Float64(h.counts[length]) + 1.0 / 40.0) / denominator
            mean += Float64(length) * raw[length]
        }
        var cumulative: Float64 = 0.0
        for (length in 1..40) {
            let q = 0.75 * raw[length] + 0.25 * Float64(length) * raw[length] / mean
            @Expect(closeEnough(h.probabilities[length], q), true)
            cumulative += q
            @Expect(closeEnough(h.penalty(length), Float64(length) * (1.0 - cumulative)), true)
        }
        @Expect(closeEnough(h.score(6, 3), h.penalty(3) - h.penalty(6)), true)
    }

    @TestCase
    public func bestFitCounterexampleIsValid(): Unit {
        let p = Pipeline()
        p.addPipe(4)
        p.addPipe(10)
        p.addPipe(6)
        p.removePipe(1)
        p.removePipe(3)
        p.addPipe(3)
        p.addPipe(4)
        p.addPipe(3)
        // Six arrivals are still in warmup; adaptation must not be claimed here.
        @Expect(p.trench, 40)
        @Expect(p.occupied, 20)
        @Expect(p.checkIndexes(), true)
    }

    @TestCase
    public func optimizedCandidatesCoverAllLengths(): Unit {
        let p = Pipeline()
        let removable = ArrayList<Int64>()
        for (holeLength in 1..91) {
            var remaining = holeLength
            while (remaining > 0) {
                let part = if (remaining > 40) { 40 } else { remaining }
                p.addPipe(part)
                removable.add(p.pipeCount)
                remaining -= part
            }
            p.addPipe(1) // separator stays in the trench
        }
        for (pid in removable) {
            p.removePipe(pid)
        }
        // Ensure full statistical influence for the search test.
        for (_ in 0..500) {
            p.history.observe(3)
        }
        @Expect(p.checkIndexes(), true)
        for (length in 1..41) {
            let expected = fullScan(p, length)
            let actual = p.chooseGap(length)
            @Expect(sameGap(actual, expected), true)
            @Expect(p.lastCandidateCount <= 41, true)
        }
    }

    @TestCase
    public func rejectsInvalidPipeAndPid(): Unit {
        let p = Pipeline()
        var rejected: Int64 = 0
        try { p.addPipe(0) } catch (_: IllegalArgumentException) { rejected += 1 }
        try { p.addPipe(41) } catch (_: IllegalArgumentException) { rejected += 1 }
        try { p.removePipe(1) } catch (_: IllegalArgumentException) { rejected += 1 }
        p.addPipe(1)
        p.removePipe(1)
        try { p.removePipe(1) } catch (_: IllegalArgumentException) { rejected += 1 }
        @Expect(rejected, 4)
        @Expect(p.checkIndexes(), true)
    }

    @TestCase
    public func hundredSmallRandomScenarios(): Unit {
        for (seed in 0..100) {
            randomScenario(seed, 200)
        }
    }

    @TestCase
    public func adaptiveRandomScenarios(): Unit {
        for (seed in 100..106) {
            randomScenario(seed, 1200)
        }
    }
}
