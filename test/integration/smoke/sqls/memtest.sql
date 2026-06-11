-- Memory-pressure probe for the auto-memory smoke test (#71).
--
-- Builds one large string_agg in a SINGLE aggregate group. A single-group
-- string aggregate cannot spill to disk, so it forces DuckDB to allocate the
-- whole buffer against its memory pool:
--   * auto-memory ON  -> max_memory is sized to the cgroup limit, so DuckDB
--     raises a graceful "Out of Memory" error well below the container ceiling
--     and the server process survives.
--   * auto-memory OFF -> max_memory defaults to (host) RAM, DuckDB allocates
--     past the cgroup limit, and the kernel OOM-kills the process (exit 137).
--
-- `rows` defaults to 3,000,000 (~3 GiB of 1000-char strings), comfortably above
-- a 1 GiB container limit and an 80%-of-1GiB (~800 MiB) max_memory.
SELECT length(string_agg(s, ',')) AS total_len
FROM (
  SELECT repeat('x', 1000) AS s
  FROM range({{#params.rows}}{{ params.rows }}{{/params.rows}}{{^params.rows}}3000000{{/params.rows}})
) t
