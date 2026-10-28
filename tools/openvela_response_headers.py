"""Bounded private webclient response-line capacity; no public ABI change."""
def apply(source):
    before='  char line[CONFIG_WEBCLIENT_MAXHTTPLINE];'
    if source.count(before)!=1:raise ValueError('unsupported webclient line storage')
    source=source.replace(before,'  /* Full response headers, including long CSP, reach the caller intact. */\n  char line[16384];')
    start=source.index('static inline int wget_parseheaders(')
    end=source.index('\nstatic ',start+1)
    part=source[start:end]
    before='ndx == CONFIG_WEBCLIENT_MAXHTTPLINE - 1'
    if part.count(before)!=1:raise ValueError('unsupported response parser')
    return source[:start]+part.replace(before,'ndx == sizeof(ws->line) - 1')+source[end:]
