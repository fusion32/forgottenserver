if type(jit) == 'table' then
	print('Using ' .. jit.version)
else
	print('Using ' .. _VERSION)
end
