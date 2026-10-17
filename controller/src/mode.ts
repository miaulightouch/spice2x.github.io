const env = (import.meta as ImportMeta & { env?: { MODE?: string } }).env;

export const isEmbeddedBuild = env?.MODE === 'embedded';
export const EMBEDDED_PROFILE_ID = 'embedded';

type PageLocation = Pick<Location, 'protocol' | 'host'>;

function pageLocation(location?: PageLocation): PageLocation | undefined {
  return location ?? (typeof window === 'undefined' ? undefined : window.location);
}

export function embeddedWebSocketEndpoint(location?: PageLocation): string | undefined {
  const page = pageLocation(location);
  const protocol = page?.protocol === 'https:' ? 'wss:' : page?.protocol === 'http:' ? 'ws:' : undefined;
  return protocol && page?.host ? `${protocol}//${page.host}/` : undefined;
}

export function embeddedHttpOrigin(location?: PageLocation): string | undefined {
  const page = pageLocation(location);
  const protocol = page?.protocol === 'https:' ? 'https:' : page?.protocol === 'http:' ? 'http:' : undefined;
  return protocol && page?.host ? `${protocol}//${page.host}` : undefined;
}

export function isEmbeddedEndpoint(endpoint: string, location?: PageLocation): boolean {
  const expected = embeddedWebSocketEndpoint(location);
  if (!expected) return false;
  try {
    const actual = new URL(endpoint);
    const target = new URL(expected);
    return actual.protocol === target.protocol && actual.host === target.host;
  } catch { return false; }
}

export function isEmbeddedProfile(profile: { id: string; endpoint: string }, location?: PageLocation): boolean {
  return profile.id === EMBEDDED_PROFILE_ID && isEmbeddedEndpoint(profile.endpoint, location);
}
