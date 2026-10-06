-- Scheduler for trusted local scripts; not a security sandbox.
local M = {}
function M.new(clock, enqueue, report)
  local self = { tasks = {}, macros = {}, alive = true }
  function self:schedule(ms, callback)
    assert(type(ms) == 'number' and ms >= 0 and ms % 1 == 0 and type(callback) == 'function')
    local task = { cancelled = false }
    self.tasks[task] = true
    function task:cancel() self.cancelled = true end
    enqueue(ms, function()
      self.tasks[task] = nil
      if not self.alive or task.cancelled then return end
      local ok, err = pcall(callback)
      if not ok then report('script callback failed: ' .. tostring(err)) end
    end)
    return task
  end
  function self:macro(ms, callback)
    assert(ms >= 50 and ms % 1 == 0 and type(callback) == 'function')
    local macro = { enabled = false, generation = 0, untilMs = 0, owner = self }
    self.macros[macro] = true
    function macro:isOff() return not self.enabled end
    function macro:isOn() return self.enabled end
    function macro:delay(duration)
      assert(type(duration) == 'number' and duration >= 0)
      self.untilMs = clock() + duration
    end
    function macro:disable()
      self.enabled = false; self.generation = self.generation + 1
      if self.pending then self.pending:cancel(); self.pending = nil end
    end
    function macro:enable()
      if self.enabled then return end
      assert(self.owner.alive, 'runtime stopped')
      self.enabled = true; self.generation = self.generation + 1
      local generation = self.generation
      local function tick()
        if not self.enabled or self.generation ~= generation then return end
        if clock() >= self.untilMs then
          local ok, err = pcall(callback, self)
          if not ok then self:disable(); report('macro disabled after error: '..tostring(err)); return end
        end
        if self.enabled and self.generation == generation then self.pending = self.owner:schedule(ms, tick) end
      end
      self.pending = self.owner:schedule(ms, tick)
    end
    return macro
  end
  function self:stop()
    self.alive = false
    for macro in pairs(self.macros) do macro:disable() end
    for task in pairs(self.tasks) do task:cancel() end
    self.tasks = {}; self.macros = {}
  end
  function self:bindings()
    local current
    return {
      macro = function(ms, callback)
        local macro = self:macro(ms, function(m)
          current = m
          local ok, err = pcall(callback, m)
          current = nil
          if not ok then error(err) end
        end)
        macro:enable()
        return macro
      end,
      schedule = function(ms, callback) return self:schedule(ms, callback) end,
      delay = function(ms) assert(current, 'delay requires a running macro'); current:delay(ms) end,
    }
  end
  return self
end
return M
