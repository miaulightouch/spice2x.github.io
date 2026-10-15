import { Rc4 } from './rc4';

export type ConnectionStatus = 'disconnected' | 'connecting' | 'connected' | 'error';
export interface Rpc {
  request(module: string, fn: string, params?: unknown[]): Promise<unknown[]>;
}
export class ApiError extends Error {
  constructor(readonly errors: string[]) {
    super(errors.join('\n'));
    this.name = 'ApiError';
  }
}
interface Pending {
  id: number;
  refresh: boolean;
  started: number;
  timer: ReturnType<typeof setTimeout>;
  resolve: (data: unknown[]) => void;
  reject: (error: Error) => void;
}

export function normalizeEndpoint(value: string): string {
  let url: URL;
  try { url = new URL(value.trim()); }
  catch { throw new Error('請輸入完整的 WebSocket 位址，例如 ws://192.168.1.10:1338。'); }
  if (!['ws:', 'wss:'].includes(url.protocol) || url.username || url.password || url.hash) {
    throw new Error('請輸入 ws:// 或 wss:// 位址，密碼請填在 API 密碼欄位。');
  }
  return url.toString();
}

/** Strictly serial RPC: concurrent sends would desynchronize SpiceAPI's shared cipher. */
export class SpiceClient implements Rpc {
  status: ConnectionStatus = 'disconnected';
  error = '';
  latency = 0;
  onStatus?: (status: ConnectionStatus, error: string) => void;
  private socket?: WebSocket;
  private cipher?: Rc4;
  private pending?: Pending;
  private buffer = new Uint8Array();
  private tail: Promise<unknown> = Promise.resolve();
  private generation = 0;
  private nextId = 1;
  private opening?: { timer: ReturnType<typeof setTimeout>; reject: (error: Error) => void };
  private refreshTimer?: ReturnType<typeof setInterval>;
  private readonly encoder = new TextEncoder();
  private readonly decoder = new TextDecoder('utf-8', { fatal: true });

  constructor(private readonly timeout = 8000, private readonly factory = (url: string) => new WebSocket(url)) {}

  async connect(endpoint: string, password: string): Promise<void> {
    const url = normalizeEndpoint(endpoint);
    this.disconnect();
    const generation = this.generation;
    this.cipher = password ? new Rc4(this.encoder.encode(password)) : undefined;
    this.setStatus('connecting');
    try {
      await new Promise<void>((resolve, reject) => {
        this.opening = {
          reject,
          timer: setTimeout(() => this.fail(new Error('連線逾時，請檢查位址、連接埠與本機網路權限。')), 20000),
        };
        const socket = this.factory(url);
        this.socket = socket;
        socket.binaryType = 'arraybuffer';
        socket.addEventListener('open', () => {
          if (generation !== this.generation) return;
          this.clearOpening();
          resolve();
        });
        socket.addEventListener('message', (event) => {
          if (generation === this.generation) this.receive(event.data);
        });
        socket.addEventListener('error', () => {
          if (generation === this.generation) this.fail(new Error('WebSocket 連線失敗。請確認機台 API、網路與瀏覽器的本機網路權限。'));
        });
        socket.addEventListener('close', () => {
          if (generation === this.generation) this.fail(new Error('機台連線已中斷，請重新連線。'));
        });
      });
      await this.enqueue('control', 'session_refresh', []);
      if (generation !== this.generation) throw new Error('連線已取消');
      this.setStatus('connected');
      this.refreshTimer = setInterval(() => {
        void this.request('control', 'session_refresh').catch(() => {});
      }, 5 * 60 * 1000);
    } catch (error) {
      if (generation === this.generation) this.fail(asError(error));
      throw error;
    }
  }

  request(module: string, fn: string, params: unknown[] = []): Promise<unknown[]> {
    if (this.status !== 'connected') return Promise.reject(new Error('請先連線至機台。'));
    return this.enqueue(module, fn, params);
  }

  disconnect(): void {
    this.terminate(new Error('連線已關閉'));
    this.setStatus('disconnected');
  }

  private enqueue(module: string, fn: string, params: unknown[]): Promise<unknown[]> {
    const generation = this.generation;
    // Freeze the request now: later UI edits must not mutate queued parameters.
    const id = this.nextId++;
    if (this.nextId > Number.MAX_SAFE_INTEGER) this.nextId = 1;
    const bytes = this.encoder.encode(JSON.stringify({ id, module, function: fn, params }) + '\0');
    const result = this.tail.then(() => {
      if (generation !== this.generation || !this.socket || this.socket.readyState !== 1) {
        throw new Error('連線已變更；此操作未送出。');
      }
      return new Promise<unknown[]>((resolve, reject) => {
        this.pending = {
          id, refresh: module === 'control' && fn === 'session_refresh',
          started: performance.now(), resolve, reject,
          timer: setTimeout(() => this.fail(new Error('API 回應逾時，已關閉連線以避免加密狀態錯位。請確認 API 密碼並重新連線。')), this.timeout),
        };
        try {
          this.socket!.send(this.cipher ? this.cipher.crypt(bytes) : bytes);
        } catch (error) { this.fail(asError(error)); }
      });
    });
    this.tail = result.catch(() => {});
    return result;
  }

  private receive(data: unknown): void {
    try {
      if (!(data instanceof ArrayBuffer) || !this.pending) throw new Error('收到非預期的 SpiceAPI 回應。');
      const raw = new Uint8Array(data);
      const chunk = this.cipher ? this.cipher.crypt(raw) : raw;
      if (this.buffer.length + chunk.length > 32 * 1024 * 1024) throw new Error('API 回應超過大小限制。');
      const merged = new Uint8Array(this.buffer.length + chunk.length);
      merged.set(this.buffer);
      merged.set(chunk, this.buffer.length);
      const end = merged.indexOf(0);
      if (end < 0) { this.buffer = merged; return; }
      if (end === 0) throw new Error('API 無法解析要求，請確認 API 密碼。');
      if (end !== merged.length - 1) throw new Error('API 回應包含額外資料，已關閉連線。');
      const response: unknown = JSON.parse(this.decoder.decode(merged.subarray(0, end)));
      if (!isRecord(response) || response.id !== this.pending.id ||
        !Array.isArray(response.errors) || !response.errors.every(e => typeof e === 'string') ||
        !Array.isArray(response.data)) throw new Error('API 回應格式或要求編號不符。');
      const pending = this.pending;
      if (pending.refresh && response.errors.length === 0) {
        if (typeof response.data[0] !== 'string' || !response.data[0]) throw new Error('API 未提供有效的會話金鑰。');
        // The response uses the OLD stream; all subsequent traffic uses the new one.
        this.cipher = new Rc4(this.encoder.encode(response.data[0]));
      }
      this.buffer = new Uint8Array();
      this.pending = undefined;
      clearTimeout(pending.timer);
      this.latency = Math.round(performance.now() - pending.started);
      if (response.errors.length) pending.reject(new ApiError(response.errors));
      else pending.resolve(response.data);
    } catch (error) { this.fail(asError(error)); }
  }

  private fail(error: Error): void {
    this.terminate(error);
    this.setStatus('error', error.message);
  }
  private terminate(error: Error): void {
    ++this.generation;
    if (this.refreshTimer) clearInterval(this.refreshTimer);
    this.refreshTimer = undefined;
    const opening = this.opening;
    this.clearOpening();
    opening?.reject(error);
    if (this.pending) { clearTimeout(this.pending.timer); this.pending.reject(error); }
    this.pending = undefined;
    this.socket?.close();
    this.socket = undefined;
    this.cipher = undefined;
    this.buffer = new Uint8Array();
    this.tail = Promise.resolve();
  }
  private clearOpening(): void {
    if (this.opening) clearTimeout(this.opening.timer);
    this.opening = undefined;
  }
  private setStatus(status: ConnectionStatus, error = ''): void {
    this.status = status;
    this.error = error;
    this.onStatus?.(status, error);
  }
}
export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}
export function asError(value: unknown): Error {
  return value instanceof Error ? value : new Error(String(value));
}
