// The one place requests to the backend are built; call sites go through this instead of fetch.
export function apiFetch(url, options) {
  return options === undefined ? fetch(url) : fetch(url, options);
}
