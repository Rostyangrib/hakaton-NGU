package pipeline

import std.collection.*
import std.env.*
import std.sort.*

let MAX_PIPES: Int64 = 200_000
let MAX_TRENCH: Int64 = 100_000_000
let WINDOW_SIZE: Int64 = 1024
let WARMUP: Int64 = 200
let RAMP: Int64 = 200
let PRIOR_MASS: Float64 = 1.0
let LENGTH_BIAS: Float64 = 0.25
let SCORE_EPS: Float64 = 1.0e-9

// Low-level reads return the currently available bytes, unlike a full-array
// ConsoleReader read. This preserves interactive request/reply ordering.
@When[os == "Windows"]
foreign func ReadFile(handle: IntNative, buffer: CPointer<UInt8>, size: UInt32,
    count: CPointer<UInt32>, overlapped: CPointer<UInt8>): Int32
@When[os == "Windows"]
foreign func GetStdHandle(kind: UInt32): IntNative
@When[os == "Windows"]
foreign func _read(fd: Int32, buffer: CPointer<UInt8>, count: UInt32): Int32
@When[os == "Windows"]
let inputHandle = unsafe { GetStdHandle(4294967286) } // STD_INPUT_HANDLE = -10
@When[os == "Windows"]
let outputHandle = unsafe { GetStdHandle(4294967285) } // STD_OUTPUT_HANDLE = -11
@When[os != "Windows"]
foreign func read(fd: Int32, buffer: CPointer<UInt8>, count: UIntNative): IntNative

@When[os == "Windows"]
func readChunk(buffer: CPointer<UInt8>, count: CPointer<UInt32>): Int64 {
    let result = unsafe { ReadFile(inputHandle, buffer, 4096, count, CPointer<UInt8>()) }
    if (result == 0) {
        // A closed anonymous pipe fails ReadFile. The rare CRT fallback
        // distinguishes EOF (0) from a read error (-1) without thread-local
        // GetLastError, which cannot survive a Cangjie foreign-call transition.
        return Int64(unsafe { _read(0, buffer, 4096) })
    }
    return Int64(unsafe { count.read() })
}
@When[os != "Windows"]
func readChunk(buffer: CPointer<UInt8>, _count: CPointer<UInt32>): Int64 {
    return Int64(unsafe { read(0, buffer, 4096) })
}

@When[os == "Windows"]
foreign func WriteFile(handle: IntNative, buffer: CPointer<UInt8>, size: UInt32,
    count: CPointer<UInt32>, overlapped: CPointer<UInt8>): Int32
@When[os != "Windows"]
foreign func write(fd: Int32, buffer: CPointer<UInt8>, count: UIntNative): IntNative

@When[os == "Windows"]
func writeChunk(buffer: CPointer<UInt8>, size: Int64, count: CPointer<UInt32>): Int64 {
    let result = unsafe { WriteFile(outputHandle, buffer, UInt32(size), count, CPointer<UInt8>()) }
    return if (result == 0) { -1 } else { Int64(unsafe { count.read() }) }
}

@When[os == "Windows"]
func configureStreams(): Unit {
    if (inputHandle == 0 || inputHandle == -1 || outputHandle == 0 || outputHandle == -1) {
        throw IllegalStateException("Cannot obtain standard stream handles")
    }
}
@When[os != "Windows"]
func configureStreams(): Unit {}
@When[os != "Windows"]
func writeChunk(buffer: CPointer<UInt8>, size: Int64, _count: CPointer<UInt32>): Int64 {
    return Int64(unsafe { write(1, buffer, UIntNative(size)) })
}

struct Gap {
    public let start: Int64
    public let length: Int64

    public init(start: Int64, length: Int64) {
        this.start = start
        this.length = length
    }
}

// Minimum positions with lazy removal. The position index is authoritative;
// stale entries are discarded only when they reach the heap root.
class StartHeap {
    public let values = ArrayList<Int64>()
    public var activeCount: Int64 = 0

    public func add(start: Int64): Unit {
        activeCount += 1
        values.add(start)
        var index = values.size - 1
        while (index > 0) {
            let parent = (index - 1) / 2
            if (values[parent] <= start) { break }
            values[index] = values[parent]
            index = parent
        }
        values[index] = start
    }

    public func popMinimum(): Unit {
        let last = values[values.size - 1]
        values.remove(at: values.size - 1)
        if (values.isEmpty()) { return }
        var index: Int64 = 0
        while (2 * index + 1 < values.size) {
            var child = 2 * index + 1
            if (child + 1 < values.size && values[child + 1] < values[child]) {
                child += 1
            }
            if (last <= values[child]) { break }
            values[index] = values[child]
            index = child
        }
        values[index] = last
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
    private let byStart = HashMap<Int64, Int64>()
    private let byEnd = HashMap<Int64, Int64>()
    private let byLength = TreeMap<Int64, StartHeap>()
    private let largeLengths = TreeSet<Int64>()
    private let shortBuckets = Array<Option<StartHeap>>(80, repeat: None)

    private func startsFor(length: Int64): Option<StartHeap> {
        if (length < 80) {
            return shortBuckets[length]
        }
        return byLength.get(length)
    }

    private func insertGap(start: Int64, length: Int64): Unit {
        if (length <= 0) {
            return
        }
        byStart[start] = length
        byEnd[start + length] = start
        if (let Some(starts) <- startsFor(length)) {
            starts.add(start)
        } else {
            let starts = StartHeap()
            starts.add(start)
            byLength[length] = starts
            if (length >= 80) {
                largeLengths.add(length)
            } else {
                shortBuckets[length] = Some(starts)
            }
        }
    }

    private func eraseGap(start: Int64, length: Int64): Unit {
        byStart.remove(start)
        byEnd.remove(start + length)
        let starts = startsFor(length).getOrThrow()
        starts.activeCount -= 1
        if (starts.activeCount == 0) {
            byLength.remove(length)
            if (length >= 80) {
                largeLengths.remove(length)
            } else {
                shortBuckets[length] = None
            }
        }
    }

    private func previousGap(start: Int64): Option<Gap> {
        // Only an exactly adjacent neighbor can merge. Do not construct a
        // directed iterator over the position index just to find this boundary.
        if (let Some(position) <- byEnd.get(start)) {
            return Some(Gap(position, byStart[position]))
        }
        return None
    }

    private func nextGap(start: Int64): Option<Gap> {
        if (let Some(length) <- byStart.get(start)) {
            return Some(Gap(start, length))
        }
        return None
    }

    private func minimumStart(length: Int64, starts: StartHeap): Int64 {
        while (!starts.values.isEmpty()) {
            let start = starts.values[0]
            if (let Some(actual) <- byStart.get(start)) {
                if (actual == length) {
                    return start
                }
            }
            starts.popMinimum()
        }
        throw IllegalStateException("No active position in a nonempty length bucket")
    }

    public func chooseGap(length: Int64): Option<Gap> {
        history.prepare()
        lastCandidateCount = 0
        var best: Option<Gap> = None
        var bestScore: Float64 = 0.0
        // Enumerate integer lengths directly. Cangjie 1.0.5's directed tree
        // iterators are too expensive for a command loop with many gaps.
        for (holeLength in length..80) {
            if (let Some(starts) <- shortBuckets[holeLength]) {
                let start = minimumStart(holeLength, starts)
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
                if (history.influence == 0.0 || holeLength >= length + 40) {
                    return best
                }
            }
        }
        // All lengths >=80 leave >=40 meters for every possible pipe length.
        // Their statistical score is equal, so only the smallest one matters.
        if (let Some(holeLength) <- largeLengths.first) {
            let starts = byLength[holeLength]
            let start = minimumStart(holeLength, starts)
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
            if (let Some(start) <- byEnd.get(trench)) {
                tailLength = byStart[start]
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
        let starts = Array<Int64>(byStart.size, repeat: 0)
        var index: Int64 = 0
        for ((start, _) in byStart) {
            starts[index] = start
            index += 1
        }
        sort(starts)
        for (start in starts) {
            result.add(Gap(start, byStart[start]))
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
        for (gap in freeGaps()) {
            let start = gap.start
            let length = gap.length
            if (length <= 0 || start < 0 || start + length > trench || start <= previousEnd) {
                return false
            }
            if (let Some(starts) <- byLength.get(length)) {
                if (!starts.values.contains(start)) {
                    return false
                }
            } else {
                return false
            }
            if (let Some(position) <- byEnd.get(start + length)) {
                if (position != start) {
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
            if (starts.activeCount <= 0) {
                return false
            }
            if ((length >= 80) != largeLengths.contains(length)) {
                return false
            }
            if (length < 80) {
                if (let Some(cached) <- shortBuckets[length]) {
                    if (cached.activeCount != starts.activeCount || cached.values.size != starts.values.size) {
                        return false
                    }
                } else {
                    return false
                }
            }
            lengthIndexCount += starts.activeCount
        }
        for (length in largeLengths) {
            if (length < 80 || !byLength.contains(length)) {
                return false
            }
        }
        for (length in 1..80) {
            if (shortBuckets[length].isSome() != byLength.contains(length)) {
                return false
            }
        }
        return count == lengthIndexCount && count == byEnd.size && freeLength + occupied == trench
    }
}

// Input commands are ASCII integers. Read only through the delimiter; never
// wait for a full block or for the next command before answering this one.
class CommandReader {
    private let buffer = LibC.malloc<UInt8>(count: 4096)
    private let readCount = LibC.malloc<UInt32>(count: 1)
    private var size: Int64 = 0
    private var offset: Int64 = 0
    private var closed = false

    public init() {
        if (buffer.isNull() || readCount.isNull()) {
            unsafe { LibC.free(buffer); LibC.free(readCount) }
            throw IllegalStateException("Cannot allocate input buffer")
        }
    }

    private func nextByte(): Int64 {
        if (offset == size) {
            size = readChunk(buffer, readCount)
            offset = 0
            if (size < 0 || size > 4096) {
                throw IllegalStateException("Cannot read input")
            }
            if (size == 0) {
                return -1
            }
        }
        let value = Int64(unsafe { (buffer + offset).read() })
        offset += 1
        return value
    }

    public func nextCommand(): Option<Int64> {
        var byte = nextByte()
        while (byte >= 0 && byte <= 32) {
            byte = nextByte()
        }
        if (byte < 0) {
            return None
        }
        var sign: Int64 = 1
        if (byte == 45 || byte == 43) {
            if (byte == 45) {
                sign = -1
            }
            byte = nextByte()
        }
        if (byte < 48 || byte > 57) {
            throw IllegalArgumentException("Expected an integer command")
        }
        var value: Int64 = 0
        while (byte >= 48 && byte <= 57) {
            value = value * 10 + byte - 48
            if (value > MAX_PIPES) {
                throw IllegalArgumentException("Command exceeds PID/length limits")
            }
            byte = nextByte()
        }
        if (byte > 32) {
            throw IllegalArgumentException("Expected whitespace after a command")
        }
        return Some(sign * value)
    }

    public func close(): Unit {
        if (!closed) {
            unsafe { LibC.free(buffer) }
            unsafe { LibC.free(readCount) }
            closed = true
        }
    }
}

class ReplyWriter {
    private let buffer = LibC.malloc<UInt8>(count: 32)
    private let writtenCount = LibC.malloc<UInt32>(count: 1)
    private var closed = false

    public init() {
        if (buffer.isNull() || writtenCount.isNull()) {
            unsafe { LibC.free(buffer); LibC.free(writtenCount) }
            throw IllegalStateException("Cannot allocate output buffer")
        }
    }

    private func digitCount(value: Int64): Int64 {
        var remaining = value
        var digits: Int64 = 1
        while (remaining >= 10) {
            remaining /= 10
            digits += 1
        }
        return digits
    }

    private func encode(start: Int64, digits: Int64, value: Int64): Unit {
        var remaining = value
        for (index in 0..digits) {
            unsafe { (buffer + start + digits - index - 1).write(UInt8(48 + remaining % 10)) }
            remaining /= 10
        }
    }

    public func placement(digs: Int64, position: Int64): Unit {
        let digits = digitCount(position)
        unsafe {
            buffer.write(UInt8(48 + digs))
            (buffer + 1).write(32)
            (buffer + digits + 2).write(10)
        }
        encode(2, digits, position)
        flushReply(digits + 3)
    }

    public func removal(position: Int64): Unit {
        let digits = digitCount(position)
        encode(0, digits, position)
        unsafe { (buffer + digits).write(10) }
        flushReply(digits + 1)
    }

    private func flushReply(size: Int64): Unit {
        // Descriptor writes have no stdio output buffer. Complete this entire
        // reply now: do not wait for another command or batch several replies.
        var offset: Int64 = 0
        while (offset < size) {
            let written = writeChunk(unsafe { buffer + offset }, size - offset, writtenCount)
            if (written <= 0 || written > size - offset) {
                throw IllegalStateException("Cannot write the interactive reply")
            }
            offset += written
        }
    }

    public func close(): Unit {
        if (!closed) {
            unsafe { LibC.free(buffer) }
            unsafe { LibC.free(writtenCount) }
            closed = true
        }
    }
}

main(): Int64 {
    configureStreams()
    let input = CommandReader()
    let output = ReplyWriter()
    try {
        let pipeline = Pipeline()
        while (let Some(command) <- input.nextCommand()) {
            if (command == 0) {
                return 0
            }
            if (command > 0) {
                let (digs, position) = pipeline.addPipe(command)
                output.placement(digs, position)
            } else {
                let position = pipeline.removePipe(-command)
                output.removal(position)
            }
        }
        return 0
    } catch (error: Exception) {
        getStdErr().writeln("Pipeline error: ${error}")
        getStdErr().flush()
        return 1
    } finally {
        input.close()
        output.close()
    }
}
