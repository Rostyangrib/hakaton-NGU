package pipeline

import std.collection.*
import std.env.*
import std.convert.*

let MAX_PIPES: Int64 = 200_000
let MAX_TRENCH: Int64 = 100_000_000
let WINDOW_SIZE: Int64 = 1024
let WARMUP: Int64 = 200
let RAMP: Int64 = 200
let PRIOR_MASS: Float64 = 1.0
let LENGTH_BIAS: Float64 = 0.25
let SCORE_EPS: Float64 = 1.0e-9

struct Gap {
    public let start: Int64
    public let length: Int64

    public init(start: Int64, length: Int64) {
        this.start = start
        this.length = length
    }
}

// Sliding history counts arrivals, even if their pipes are removed later.
class ArrivalHistory {
    public let counts = Array<Int64>(41, repeat: 0)
    public let probabilities = Array<Float64>(41, repeat: 0.0)
    public let penalties = Array<Float64>(41, repeat: 0.0)
    private let ring = Array<Int64>(WINDOW_SIZE, repeat: 0)
    public var seen: Int64 = 0
    public var size: Int64 = 0
    private var cursor: Int64 = 0
    public var influence: Float64 = 0.0

    public func observe(length: Int64): Unit {
        if (length < 1 || length > 40) {
            throw IllegalArgumentException("Pipe length must be in 1..40")
        }
        if (size == WINDOW_SIZE) {
            counts[ring[cursor]] -= 1
        } else {
            size += 1
        }
        ring[cursor] = length
        cursor = (cursor + 1) % WINDOW_SIZE
        counts[length] += 1
        seen += 1
    }

    // Call before placing the next pipe. It is not in the histogram yet.
    public func prepare(): Unit {
        let t = seen + 1
        influence = if (t <= WARMUP) {
            0.0
        } else if (t >= WARMUP + RAMP) {
            1.0
        } else {
            Float64(t - WARMUP) / Float64(RAMP)
        }
        if (influence == 0.0) {
            return
        }

        let denominator = Float64(size) + PRIOR_MASS
        var mean: Float64 = 0.0
        for (length in 1..41) {
            let p = (Float64(counts[length]) + PRIOR_MASS / 40.0) / denominator
            probabilities[length] = p
            mean += Float64(length) * p
        }
        for (length in 1..41) {
            let p = probabilities[length]
            probabilities[length] = (1.0 - LENGTH_BIAS) * p +
                LENGTH_BIAS * Float64(length) * p / mean
        }

        // A suffix sum avoids cancellation in 1 - F(g).
        var tail: Float64 = 0.0
        penalties[40] = 0.0
        for (index in 0..39) {
            let g = 39 - index
            tail += probabilities[g + 1]
            penalties[g] = Float64(g) * tail
        }
        penalties[0] = 0.0
    }

    public func penalty(length: Int64): Float64 {
        if (length <= 0 || length >= 40) {
            return 0.0
        }
        return penalties[length]
    }

    public func score(hole: Int64, pipe: Int64): Float64 {
        let rest = hole - pipe
        return (1.0 - influence) * Float64(rest) +
            influence * (penalty(rest) - penalty(hole))
    }
}

class Pipeline {
    public let history = ArrivalHistory()
    public var trench: Int64 = 0
    public var pipeCount: Int64 = 0
    public var occupied: Int64 = 0
    public var lastCandidateCount: Int64 = 0
    private let positions = Array<Int64>(MAX_PIPES + 1, repeat: -1)
    private let lengths = Array<Int64>(MAX_PIPES + 1, repeat: 0)
    private let byStart = TreeMap<Int64, Int64>()
    private let byLength = TreeMap<Int64, TreeSet<Int64>>()

    private func insertGap(start: Int64, length: Int64): Unit {
        if (length <= 0) {
            return
        }
        byStart[start] = length
        if (let Some(starts) <- byLength.get(length)) {
            starts.add(start)
        } else {
            let starts = TreeSet<Int64>()
            starts.add(start)
            byLength[length] = starts
        }
    }

    private func eraseGap(start: Int64, length: Int64): Unit {
        byStart.remove(start)
        let starts = byLength[length]
        starts.remove(start)
        if (starts.isEmpty()) {
            byLength.remove(length)
        }
    }

    private func previousGap(start: Int64): Option<Gap> {
        // Cangjie 1.0.5 throws on directed iteration of an empty TreeMap.
        if (byStart.isEmpty() || byStart.first.getOrThrow()[0] >= start) {
            return None
        }
        if (let Some((position, length)) <- byStart.backward(start, inclusive: false).next()) {
            return Some(Gap(position, length))
        }
        return None
    }

    private func nextGap(start: Int64): Option<Gap> {
        if (byStart.isEmpty() || byStart.last.getOrThrow()[0] < start) {
            return None
        }
        if (let Some((position, length)) <- byStart.forward(start).next()) {
            return Some(Gap(position, length))
        }
        return None
    }

    public func chooseGap(length: Int64): Option<Gap> {
        history.prepare()
        lastCandidateCount = 0
        if (byLength.isEmpty() || byLength.last.getOrThrow()[0] < length) {
            return None
        }
        let candidates = byLength.forward(length)
        var best: Option<Gap> = None
        var bestScore: Float64 = 0.0
        while (let Some((holeLength, starts)) <- candidates.next()) {
            let start = starts.first.getOrThrow()
            let score = history.score(holeLength, length)
            lastCandidateCount += 1
            if (let Some(old) <- best) {
                if (score < bestScore - SCORE_EPS ||
                    (score <= bestScore + SCORE_EPS &&
                        (holeLength < old.length || (holeLength == old.length && start < old.start)))) {
                    best = Some(Gap(start, holeLength))
                    bestScore = score
                }
            } else {
                best = Some(Gap(start, holeLength))
                bestScore = score
            }
            // During warmup the first suitable length is ordinary Best Fit.
            // For larger holes both Phi(h) and Phi(h-x) are zero; only the
            // smallest representative can improve the deterministic tie break.
            if (history.influence == 0.0 || holeLength >= length + 40) {
                break
            }
        }
        return best
    }

    public func addPipe(length: Int64): (Int64, Int64) {
        if (length < 1 || length > 40 || pipeCount >= MAX_PIPES) {
            throw IllegalArgumentException("Invalid pipe length or too many pipes")
        }
        let gap = chooseGap(length)
        var digs: Int64 = 0
        var position: Int64 = 0
        if (let Some(hole) <- gap) {
            position = hole.start
            eraseGap(hole.start, hole.length)
            insertGap(hole.start + length, hole.length - length)
        } else {
            var tailLength: Int64 = 0
            if (let Some((start, freeLength)) <- byStart.last) {
                if (start + freeLength == trench) {
                    tailLength = freeLength
                }
            }
            digs = (length - tailLength + 19) / 20
            let newTrench = trench + digs * 20
            if (newTrench > MAX_TRENCH) {
                throw IllegalStateException("Trench length exceeds protocol limit")
            }
            position = trench - tailLength
            if (tailLength > 0) {
                eraseGap(position, tailLength)
            }
            trench = newTrench
            insertGap(position + length, trench - position - length)
        }
        pipeCount += 1
        positions[pipeCount] = position
        lengths[pipeCount] = length
        occupied += length
        history.observe(length)
        return (digs, position)
    }

    public func removePipe(pid: Int64): Int64 {
        if (pid < 1 || pid > pipeCount || lengths[pid] == 0) {
            throw IllegalArgumentException("Unknown or already removed PID")
        }
        let position = positions[pid]
        var start = position
        var length = lengths[pid]
        let left = previousGap(start)
        let right = nextGap(start + length)
        if (let Some(hole) <- left) {
            if (hole.start + hole.length == start) {
                eraseGap(hole.start, hole.length)
                start = hole.start
                length += hole.length
            }
        }
        if (let Some(hole) <- right) {
            if (hole.start == start + length) {
                eraseGap(hole.start, hole.length)
                length += hole.length
            }
        }
        insertGap(start, length)
        occupied -= lengths[pid]
        lengths[pid] = 0
        return position
    }

    // Snapshots are used only by unit tests, not in the command loop.
    public func freeGaps(): ArrayList<Gap> {
        let result = ArrayList<Gap>()
        for ((start, length) in byStart) {
            result.add(Gap(start, length))
        }
        return result
    }

    public func pipePosition(pid: Int64): Int64 {
        return positions[pid]
    }

    public func pipeLength(pid: Int64): Int64 {
        return lengths[pid]
    }

    public func checkIndexes(): Bool {
        var previousEnd: Int64 = -1
        var freeLength: Int64 = 0
        var count: Int64 = 0
        for ((start, length) in byStart) {
            if (length <= 0 || start < 0 || start + length > trench || start <= previousEnd) {
                return false
            }
            if (let Some(starts) <- byLength.get(length)) {
                if (!starts.contains(start)) {
                    return false
                }
            } else {
                return false
            }
            previousEnd = start + length
            freeLength += length
            count += 1
        }
        var lengthIndexCount: Int64 = 0
        for ((length, starts) in byLength) {
            if (starts.isEmpty()) {
                return false
            }
            for (start in starts) {
                if (let Some(actual) <- byStart.get(start)) {
                    if (actual != length) {
                        return false
                    }
                } else {
                    return false
                }
                lengthIndexCount += 1
            }
        }
        return count == lengthIndexCount && freeLength + occupied == trench
    }
}

main(): Int64 {
    try {
        let pipeline = Pipeline()
        let input = getStdIn()
        let output = getStdOut()
        while (let Some(line) <- input.readln()) {
            let command = Int64.parse(line)
            if (command == 0) {
                return 0
            }
            if (command > 0) {
                let (digs, position) = pipeline.addPipe(command)
                output.writeln("${digs} ${position}")
            } else {
                let position = pipeline.removePipe(-command)
                output.writeln("${position}")
            }
            output.flush()
        }
        return 0
    } catch (error: Exception) {
        getStdErr().writeln("Pipeline error: ${error}")
        getStdErr().flush()
        return 1
    }
}
