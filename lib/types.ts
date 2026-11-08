// ─── Capture Targets ────────────────────────────────────────────

export interface MonitorTarget {
  type: 'monitor'
  index: number
}

export interface WindowTitleTarget {
  type: 'windowTitle'
  pattern: string
}

export interface WindowExeTarget {
  type: 'windowExe'
  pattern: string
}

export type CaptureTarget = MonitorTarget | WindowTitleTarget | WindowExeTarget

// ─── Key Tracking ───────────────────────────────────────────────

export interface KeySpec {
  bit: number
  vk: number
  name: string
}

// ─── Engine Options ─────────────────────────────────────────────

export interface EngineOptions {
  target: CaptureTarget
  maxFps?: number
  queueCapacity?: number
  captureCursor?: boolean
  keys?: KeySpec[]
  recordWidth?: number
  recordHeight?: number
  recordGop?: number
}

// ─── Frame Packet ───────────────────────────────────────────────

export interface FramePacket {
  readonly frameId: number
  readonly width: number
  readonly height: number
  readonly stride: number
  readonly captureQpc: bigint
  readonly hostAcceptQpc: bigint
  readonly keyboardMask: bigint
  readonly data: Buffer
  readonly released: boolean
  release(): void
}

// ─── Recording ──────────────────────────────────────────────────

export interface RecordingInfo {
  basePath: string
  videoPath: string
  metaPath: string
  codec: string
  width: number
  height: number
}

// ─── Stats ──────────────────────────────────────────────────────

export interface EngineStats {
  framesSeen: number
  framesAccepted: number
  framesDroppedQueueFull: number
  framesDroppedError: number
  framesRecorded: number
  queueDepth: number
  recording: boolean
  /** Times the frame pool was resized to a new window ContentSize. */
  poolRecreates: number
  /** Current frame pool size. */
  poolWidth: number
  poolHeight: number
  /** Most recent pool resize, e.g. "1920x1080 -> 1904x1041" ("" if none). */
  lastPoolChange: string
  /** D3D adapter the capture device was created on. */
  adapter: string
  /** Frame errors per failing step, e.g. "Recreate=296, Map=2" ("" if none). */
  errorsByStep: string
}

// ─── Meta Format ────────────────────────────────────────────────

export interface MetaHeader {
  version: number
  createdUnixNs: bigint
  keyCount: number
}

export interface MetaKeyEntry {
  bit: number
  vk: number
  name: string
}

export interface MetaRow {
  frameId: bigint
  recordFrameIndex: bigint
  captureQpc: bigint
  hostAcceptQpc: bigint
  keyboardMask: bigint
  width: number
  height: number
  analysisStride: number
  flags: number
}

export interface MetaFile {
  header: MetaHeader
  keys: MetaKeyEntry[]
  rows: MetaRow[]
}
