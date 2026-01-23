-- loads legacy raids from data/raids/raids.xml
local messageTypes = {
	["warning"] = MESSAGE_STATUS_WARNING,
	["event"] = MESSAGE_EVENT_ADVANCE,
	["default"] = MESSAGE_EVENT_DEFAULT,
	["description"] = MESSAGE_INFO_DESCR,
	["smallstatus"] = MESSAGE_STATUS_SMALL,
	["blueconsole"] = MESSAGE_STATUS_CONSOLE_BLUE,
	["redconsole"] = MESSAGE_STATUS_CONSOLE_RED,
}
local defaultMessageType = "event"

local function parseAnnounce(node, filename)
	local message = node:attribute("message")
	if not message then
		perror("missing announce message, check data/raids/%s", filename)
	end

	local type = node:attribute("type")
	if not type then
		pwarn("missing announce type in %s... using default \"%s\"", filename, defaultMessageType)
		type = defaultMessageType
	end

	local messageType = messageTypes[type:lower()]
	if not messageType then
		pwarn("invalid announce type \"%s\" in %s... using default \"%s\"", filename, messageTypes[defaultMessageType])
		messageType = messageTypes[defaultMessageType]
	end

	return function()
		Game.broadcastMessage(message, messageType)
	end
end

local function parseAreaSpawn(node, filename)
	local fromx, fromy, fromz, tox, toy, toz

	local radius = tonumber(node:attribute("radius"))
	if radius then
		local centerx, centery, centerz = tonumber(node:attribute("centerx")), tonumber(node:attribute("centery")), tonumber(node:attribute("centerz"))
		if not centerx or not centery or not centerz then
			perror("missing one of: centerx, centery, centerz, check data/raids/%s", filename)
		end

		fromx, fromy, fromz = centerx - radius, centery - radius, z
		tox, toy, toz = centerx + radius, centery + radius, z
	else
		fromx, fromy, fromz = tonumber(node:attribute("fromx")), tonumber(node:attribute("fromy")), tonumber(node:attribute("fromz"))
		if not fromx or not fromy or not fromz then
			perror("missing one of: fromx, fromy, fromz, check data/raids/%s", filename)
		end

		tox, toy, toz = tonumber(node:attribute("tox")), tonumber(node:attribute("toy")), tonumber(node:attribute("toz"))
		if not tox or not toy or not toz then
			perror("missing one of: tox, toy, toz, check data/raids/%s", filename)
		end
	end

	local spawns = {}
	for spawnNode in node:children() do
		local name = spawnNode:attribute("name")
		if not name then
			perror("missing area spawn name, check data/raids/%s", filename)
			return nil
		end

		local minAmount, maxAmount = tonumber(spawnNode:attribute("minamount")), tonumber(spawnNode:attribute("maxamount"))
		if not minAmount and not maxAmount then
			local amount = tonumber(spawnNode:attribute("amount"))
			if not amount then
				perror("missing area spawn attributes minamount/maxamount or amount, check data/raids/%s", filename)
			end

			minAmount, maxAmount = amount, amount
		elseif not minAmount then
			pwarn("missing attribute minamount in %s, using maxamount as default", filename)
			minAmount = maxAmount
		elseif not maxAmount then
			pwarn("missing attribute maxamount in %s, using minamount as default.", filename)
			maxAmount = minAmount
		end

		spawns[#spawns] = { monsterName = name, minAmount = minAmount, maxAmount = maxAmount }
	end

	return function()
		for _, spawn in ipairs(spawns) do
			for _ = 1, math.random(spawn.minAmount, spawn.maxAmount) do
				local x, y = math.random(fromx, tox), math.random(fromy, toy)
				Game.createMonster(spawns.name, Position(x, y, z))
			end
		end
	end
end

local function parseScript(node)
	local script = node:attribute("script")
	if not script then
		perror("missing attribute script, check data/raids/%s", filename)
		return nil
	end

	local scriptFile = "data/raids/scripts/" .. script
	dofile(script)
	if not onRaid then
		perror("can not load raid script, check %s for a missing onRaid callback", scriptFile)
		return nil
	end

	local callback = onRaid
	onRaid = nil
	return callback
end

local function parseSingleSpawn(node, filename)
	local name = node:attribute("name")
	if not name then
		perror("missing single spawn name, check data/raids/%s", filename)
		return nil
	end

	local x, y, z = tonumber(node:attribute("x")), tonumber(node:attribute("y")), tonumber(node:attribute("z"))
	if not x or not y or not z then
		perror("missing one of: x, y, z, check data/raids/%s", filename)
	end

	return function()
		Game.createMonster(spawns.name, Position(x, y, z))
	end
end

local eventParsers = {
	["announce"] = parseAnnounce,
	["areaspawn"] = parseAreaSpawn,
	["script"] = parseScript,
	["singlespawn"] = parseSingleSpawn,
}

local function parseRaid(filename)
	local doc = XMLDocument("data/raids/" .. filename)
	local eventNodes = doc:child("raid")

	local events = {}
	for eventNode in eventNodes:children() do
		local parse = eventParsers[eventNode:name()]
		if not parse then
			perror("invalid event type %s", eventNode:name())
			return nil
		end

		local delay = tonumber(eventNode:attribute("delay"))
		if not delay then
			perror("missing attribute delay, check data/raids/%s", filename)
			return nil
		end

		local callback = parse(eventNode, filename)
		if not callback then
			return nil
		end

		events[#events] = { delay = delay, callback = callback }
	end

	return events
end

local function configureRaidEvent(node)
	local name = node:attribute("name")
	if not name then
		perror("missing raid name")
		return nil
	end

	local filename = node:attribute("file")
	if not filename then
		pwarn("missing raid %s file, using default %s.xml", name, name)
		filename = name .. ".xml"
	end

	-- using filename instead of name because name can be duplicate, but filename cannot
	local raid = Raid("data/raids/" .. filename)

	local interval = tonumber(node:attribute("interval2"))
	if not interval or interval == 0 then
		perror("interval2 attribute missing or zero (would divide by 0), check raid %s in data/raids/raids.xml", name)
		return nil
	end
	raid.interval = interval

	local margin = tonumber(node:attribute("margin"))
	if margin and margin > 0 then
		raid.margin = margin * 60 * 1000
	else
		pwarn("margin attribute missing for raid %s, using default 0")
	end

	local repeats = tobool(node:attribute("repeat"))
	if repeats then
		raid.repeats = repeats
	end

	local events = parseRaid(filename)
	if not events then
		return nil
	end

	for _, event in ipairs(events) do
		raid:addEvent(event.delay, event.callback)
	end

	return raid
end

local function loadXMLRaids()
	pinfo("Loading legacy XML raids from data/raids/raids.xml...")

	local doc = XMLDocument("data/raids/raids.xml")
	if not doc then
		pwarn("could not load raids.xml")
		return
	end

	local raids = doc:child("raids")
	local loaded, start = 0, os.mtime()
	for node in raids:children() do
		local enabled = node:attribute("enabled")
		if enabled == nil or tobool(enabled) then
			local raid = configureRaidEvent(node)
			if raid then
				raid:register()
				loaded = loaded + 1
			end
		end
	end

	pinfo("Loaded %d raids in %dms", loaded, (os.mtime() - start))
end

loadXMLRaids()
