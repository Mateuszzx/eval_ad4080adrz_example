# west flash finishes with "reset run". On this ST-Link that command drops
# SWD after the image is already programmed. The core is halted by
# "reset init", so resume starts the image that was just written.
rename reset _sdp_k1_reset_orig

proc reset {args} {
	if {[llength $args] == 1 && [lindex $args 0] eq "run"} {
		resume
		return
	}

	_sdp_k1_reset_orig {*}$args
}
