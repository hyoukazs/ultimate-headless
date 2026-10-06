-- Canonical raw-connection helper, loaded by the headless allowlist.
-- Classificação: "refused" = permanente (sem retry); eof/reset/timeout =
-- transitórios com backoff exponencial. Política do protótipo, não contrato
-- de macros futuros. Login errors need a separate authenticated classifier.
local M = {}

local function permanent(msg)
  return msg ~= nil and msg:find("refused", 1, true) ~= nil
end

function M.connect(opts)
  local attempt = 0
  local delay = opts.baseMs or 100
  local function try()
    attempt = attempt + 1
    log("reconnect attempt=" .. attempt)
    netOnError(function(err)
      if permanent(err) then
        log("reconnect permanent: " .. err)
        opts.onFailed(err, attempt)
      elseif attempt >= (opts.maxAttempts or 5) then
        log("reconnect exhausted: " .. err)
        opts.onFailed(err, attempt)
      else
        log("reconnect retry in " .. delay .. "ms: " .. err)
        later(delay, try)
        delay = math.min(delay * 2, opts.maxMs or 1000)
      end
    end)
    netConnect(opts.host, opts.port, opts.onConnected)
  end
  try()
end

return M
