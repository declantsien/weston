function configure_surface(s)
	ivi.ivi_print("configure_surface")

	if s.width == 0 or s.height == 0 then
		s.width = 400
		s.height = 800
	end

	-- TODO Assign a surface to an output
	--outputs["wayland-0"]:add(surface)
	--s.x = 200
	--s.y = 300
end

-- TODO Register a callback function to a signal
--ivi:on("configure_surface", configure_surface)
