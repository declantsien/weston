function configure_surface(s)
	ivi.ivi_print("configure_surface")

	if s.width == 0 or s.height == 0 then
		s.width = 400
		s.height = 800
	end
end
