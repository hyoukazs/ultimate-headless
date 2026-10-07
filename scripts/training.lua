-- User-requested training. api must expose verified live game state.
-- This module owns no connection and must be stopped before its state is destroyed.
local M = {}
function M.start(api, runtime)
  for _, name in ipairs({'online','name','say','manaPercent','inPz','attacking',
                          'position','spectators','attack','onTalk','playerId','attackId','followId','follow','cancel'}) do
    assert(type(api[name]) == 'function', 'missing live API: '..name)
  end
  local self = { treinando = false, alive = true }
  local function online() return self.alive and api.online() end
  local function sameFloor(a,b) return a and b and a.z == b.z end
  local function distance(a,b) return math.max(math.abs(a.x-b.x), math.abs(a.y-b.y)) end
  self.AntiPush = runtime:macro(100, function()
    if online() and not self.treinando then api.say('!treinar') end
  end)
  local unsubscribe = api.onTalk(function(name, level, mode, text)
    if online() and name == api.name() and type(text) == 'string' and text:lower() == 'on!' then
      self.treinando = true
    end
  end)
  assert(type(unsubscribe) == 'function', 'onTalk must return an unsubscribe function')
  self.cancelTraining = runtime:macro(100, function()
    if online() and self.AntiPush:isOff() and self.treinando then
      api.say('Kai'); self.treinando = false
    end
  end)
  self.chakra = runtime:macro(100, function()
    if not online() then return end
    -- mppercent compatibility alias is supplied by the live adapter.
    local mana = api.manaPercent()
    if type(mana) == 'number' and mana == mana and mana > 45 and mana < math.huge then
      api.say('powerdown')
    end
  end)
  -- Training buff macro (e.g., Byakugan Tenken). TRAINING_BUFF is set by C++ before this
  -- module loads. Says the buff every 3 s while online, as requested by the user.
  local trainingBuff = TRAINING_BUFF
  if type(trainingBuff) == 'string' and trainingBuff ~= '' then
    self.trainingBuff = runtime:macro(3000, function()
      if online() then
        api.say(trainingBuff)
      end
    end)
  end
  self.target = runtime:macro(200, function()
    if not online() or type(api.inPz())~='boolean' then return end
    local pos = api.position()
    if not pos then return end
    local best, bestDistance
    local spectators=api.spectators() or {}
    local function occupied(creature)
      for _, other in pairs(spectators) do
        if other.player==true and other.id~=api.playerId()
          and sameFloor(other.position,creature.position)
          and distance(other.position,creature.position)<=1 then return true end
      end
      return false
    end
    for _, creature in pairs(spectators) do
      if creature.monster == true and type(creature.name) == 'string'
         and creature.name:lower() == 'trainer' and sameFloor(pos, creature.position)
         and type(creature.id) == 'number' and creature.id > 0 then
        local d = distance(pos, creature.position)
        if d <= 7 and not occupied(creature) and (not best or d < bestDistance or (d == bestDistance and creature.id < best.id)) then
          best, bestDistance = creature, d
        end
      end
    end
    local attackId,followId=api.attackId(),api.followId()
    if not best then
      if attackId~=0 or followId~=0 then api.cancel() end
    elseif api.inPz() then
      if attackId~=0 then api.cancel() end
      if followId~=best.id or attackId~=0 then api.follow(best.id) end
    else
      if followId~=0 then api.cancel() end
      if attackId~=best.id or followId~=0 then api.attack(best.id) end
    end
  end)
  function self:stop()
    if not self.alive then return end
    self.alive = false
    unsubscribe()
    for _, m in ipairs({self.AntiPush,self.cancelTraining,self.chakra,self.target,self.trainingBuff}) do m:disable() end
    self.treinando = false
  end
  for _, m in ipairs({self.AntiPush,self.cancelTraining,self.chakra,self.target,self.trainingBuff}) do m:enable() end
  return self
end
return M
