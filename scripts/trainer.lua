local M = {}
-- Core creature identities; cancelTraining cancels only script-owned actions.
function M.create(runtime, api, name)
  assert(type(name) == 'string' and name ~= '', 'exact trainer name required')
  local previousId, previousMode
  local function clear()
    if previousId then api.cancelTraining() end
    previousId, previousMode = nil, nil
  end
  local macro = runtime:macro(250, function()
    if not api.isOnline() then clear(); return end
    local player = api.getLocalPlayer()
    if not player or not player:getPosition() then clear(); return end
    local position = player:getPosition()
    local best, distance
    for _, creature in ipairs(api.getSpectators()) do
      local p = creature:getPosition()
      if creature:getId() ~= player:getId() and creature:getName() == name and p and p.z == position.z then
        local d = math.max(math.abs(p.x-position.x), math.abs(p.y-position.y))
        if not best or d < distance or (d == distance and creature:getId() < best:getId()) then best, distance = creature, d end
      end
    end
    if not best then clear(); return end
    local mode = distance > 1 and 'follow' or 'attack'
    if best:getId() == previousId and mode == previousMode then return end
    if mode == 'follow' then api.cancelTraining(); api.follow(best)
    else api.cancelFollow(); api.attack(best) end
    previousId, previousMode = best:getId(), mode
  end)
  local disable = macro.disable
  function macro:disable() clear(); disable(self) end
  return macro
end
return M
