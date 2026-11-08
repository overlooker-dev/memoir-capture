import { describe, it, expect, afterEach } from 'vitest'
import { CaptureEngine, ping, version, grab } from '../../lib/native'
import { readMeta } from '../../lib/meta'
import { mkdtempSync, rmSync, existsSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'

// These tests require a display and the native addon.
// Skip in CI with: vitest run tests/node/meta.test.ts

let tmpDir: string

afterEach(() => {
  if (tmpDir) {
    rmSync(tmpDir, { recursive: true, force: true })
  }
})

describe('module-level', () => {
  it('ping returns expected string', () => {
    expect(ping()).toBe('memoir-node 0.3.2 loaded OK')
  })

  it('version matches', () => {
    expect(version).toBe('0.3.2')
  })
})

describe('CaptureEngine', () => {
  it('captures frames from monitor', () => {
    const engine = new CaptureEngine({
      target: { type: 'monitor', index: 0 },
      maxFps: 10,
    })

    engine.start()

    try {
      for (let i = 0; i < 5; i++) {
        const frame = engine.getNextFrame(5000)
        expect(frame).not.toBeNull()
        if (!frame) break

        expect(frame.width).toBeGreaterThan(0)
        expect(frame.height).toBeGreaterThan(0)
        expect(frame.stride).toBeGreaterThanOrEqual(frame.width * 4)
        expect(frame.data.length).toBe(frame.stride * frame.height)
        expect(frame.frameId).toBeGreaterThanOrEqual(0)
        expect(typeof frame.keyboardMask).toBe('bigint')
        expect(typeof frame.captureQpc).toBe('bigint')
        expect(typeof frame.hostAcceptQpc).toBe('bigint')

        frame.release()
        expect(frame.released).toBe(true)
      }
    } finally {
      engine.stop()
    }
  })

  it('getNextFrame(0) returns null when queue empty', () => {
    const engine = new CaptureEngine({
      target: { type: 'monitor', index: 0 },
      maxFps: 1,
    })

    engine.start()

    try {
      // Drain the queue first
      while (engine.getNextFrame(0) !== null) {
        // drain
      }
      // Poll should return null immediately
      const frame = engine.getNextFrame(0)
      expect(frame).toBeNull()
    } finally {
      engine.stop()
    }
  })

  it('frame IDs are monotonically increasing', () => {
    const engine = new CaptureEngine({
      target: { type: 'monitor', index: 0 },
      maxFps: 10,
    })

    engine.start()
    const frameIds: number[] = []

    try {
      for (let i = 0; i < 5; i++) {
        const frame = engine.getNextFrame(5000)
        if (!frame) break
        frameIds.push(frame.frameId)
        frame.release()
      }
    } finally {
      engine.stop()
    }

    for (let i = 1; i < frameIds.length; i++) {
      expect(frameIds[i]).toBeGreaterThan(frameIds[i - 1])
    }
  })

  it('records to mp4 and meta', () => {
    tmpDir = mkdtempSync(join(tmpdir(), 'memoir-rec-'))
    const basePath = join(tmpDir, 'test_recording')

    const engine = new CaptureEngine({
      target: { type: 'monitor', index: 0 },
      maxFps: 10,
    })

    engine.start()

    try {
      const info = engine.startRecording(basePath)
      expect(info.videoPath).toContain('.mp4')
      expect(info.metaPath).toContain('.meta')
      expect(info.codec).toBeTruthy()
      expect(info.width).toBeGreaterThan(0)
      expect(info.height).toBeGreaterThan(0)
      expect(engine.isRecording()).toBe(true)

      for (let i = 0; i < 10; i++) {
        const frame = engine.getNextFrame(5000)
        expect(frame).not.toBeNull()
        frame!.release()
      }

      engine.stopRecording()
      expect(engine.isRecording()).toBe(false)

      // Verify files
      expect(existsSync(info.videoPath)).toBe(true)
      expect(existsSync(info.metaPath)).toBe(true)

      // Read and verify meta
      const meta = readMeta(info.metaPath)
      expect(meta.header.version).toBe(1)
      expect(meta.keys.length).toBeGreaterThan(0)
      expect(meta.rows.length).toBe(10)

      for (let i = 0; i < meta.rows.length; i++) {
        expect(meta.rows[i].recordFrameIndex).toBe(BigInt(i))
      }
    } finally {
      engine.stop()
    }
  })

  it('stats returns valid counters', () => {
    const engine = new CaptureEngine({
      target: { type: 'monitor', index: 0 },
      maxFps: 10,
    })

    engine.start()

    try {
      // Get a few frames to populate stats
      for (let i = 0; i < 3; i++) {
        const frame = engine.getNextFrame(5000)
        if (frame) frame.release()
      }

      const s = engine.stats()
      expect(s.framesAccepted).toBeGreaterThanOrEqual(3)
      expect(s.framesSeen).toBeGreaterThanOrEqual(s.framesAccepted)
      expect(typeof s.framesDroppedQueueFull).toBe('number')
      expect(typeof s.framesDroppedError).toBe('number')
      expect(typeof s.framesRecorded).toBe('number')
      expect(typeof s.queueDepth).toBe('number')
      expect(typeof s.recording).toBe('boolean')
    } finally {
      engine.stop()
    }
  })

  it('accessing data after release throws', () => {
    const engine = new CaptureEngine({
      target: { type: 'monitor', index: 0 },
      maxFps: 10,
    })

    engine.start()

    try {
      const frame = engine.getNextFrame(5000)
      expect(frame).not.toBeNull()
      if (!frame) return

      frame.release()
      expect(frame.released).toBe(true)
      expect(() => frame.data).toThrow()
    } finally {
      engine.stop()
    }
  })
})

describe('grab', () => {
  it('captures a single frame', () => {
    const frame = grab({ type: 'monitor', index: 0 }, { timeoutMs: 5000 })
    expect(frame).not.toBeNull()
    expect(frame.width).toBeGreaterThan(0)
    expect(frame.height).toBeGreaterThan(0)
    frame.release()
  })
})
