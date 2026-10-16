/** SpiceAPI uses one continuous RC4 stream for BOTH directions on a connection. */
export class Rc4 {
  private readonly state = Uint8Array.from({ length: 256 }, (_, i) => i);
  private i = 0;
  private j = 0;

  constructor(key: Uint8Array) {
    if (!key.length) throw new Error('RC4 key cannot be empty');
    let j = 0;
    for (let i = 0; i < 256; i++) {
      j = (j + this.state[i]! + key[i % key.length]!) & 255;
      [this.state[i], this.state[j]] = [this.state[j]!, this.state[i]!];
    }
  }

  crypt(input: Uint8Array): Uint8Array {
    const out = new Uint8Array(input.length);
    for (let n = 0; n < input.length; n++) {
      this.i = (this.i + 1) & 255;
      this.j = (this.j + this.state[this.i]!) & 255;
      [this.state[this.i], this.state[this.j]] = [this.state[this.j]!, this.state[this.i]!];
      out[n] = input[n]! ^ this.state[(this.state[this.i]! + this.state[this.j]!) & 255]!;
    }
    return out;
  }
}
