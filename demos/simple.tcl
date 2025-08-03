package require Tk
package require tcluvc

# callback: (un)plugged device
proc devicecb {but img op dev} {
    set dev [uvc info]
    if {$dev eq ""} {
	init $but $img
    } else {
	uvc close $dev
	$but configure -text "No Camera"
    }
}

# callback: image ready or error
proc imagecb {but img dev} {
    uvc image $dev $img
}

# button handler: start/stop image capture
proc startstop {but} {
    set dev [uvc info]
    if {$dev eq ""} {
	return
    }
    switch -glob -- [uvc state $dev] {
	capture {
	    uvc stop $dev
	    $but configure -text "Start"
	}
	* {
	    uvc start $dev
	    $but configure -text "Stop"
	}
    }
}

# init device
proc init {but img {name {}}} {
    if {$name eq ""} {
	set name [lindex [uvc devices] 0]
    }
    if {$name eq ""} {
	$but configure -text "No Camera"
	return
    }
    set dev [uvc open $name [list imagecb $but $img]]
    array set d [uvc listformats $dev]
    lassign [uvc format $dev] index fps
    if {[info exists d($index)]} {
	array set f $d($index)
	if {[scan $f(frame-size) %dx%d w h] == 2} {
	    $img configure -width $w -height $h
	    $img blank
	}
    }
    $but configure -text "Start"
}

proc changemirror {w} {
    set dev [uvc info]
    if {$dev ne ""} {
	lassign [uvc mirror $dev] x y
	set n [expr {$x + $y * 2 + 1}]
	uvc mirror $dev [expr {$n & 1}] [expr {$n & 2}]
    }
}

proc changeorientation {w} {
    set dev [uvc info]
    if {$dev ne ""} {
	set n [uvc orientation $dev]
	set n [expr {($n + 90) % 360}]
	uvc orientation $dev $n
	set img [$w cget -image]
	$img configure -width 1 -height 1
	$img configure -width 0 -height 0
    }
}

proc changeparam {which dir {index {}}} {
    set dev [uvc info]
    array set p [uvc parameters $dev]
    if {[info exists p(${which}-abs)]} {
	if {[info exists p(${which}-abs-step)]} {
	    if {$index ne {}} {
		set step [lindex [split $p(${which}-abs-step) ,] $index]
	    } else {
		set step $p(${which}-abs-step)
	    }
	    set dir [expr {$dir * $step}]
	}
	if {$index ne {}} {
	    set vals [split $p(${which}-abs) ,]
	    set newval [expr {[lindex $vals $index] + $dir}]
	    set vals [lreplace $vals $index $index $newval]
	    catch {uvc parameters $dev ${which}-abs [join $vals ,]}
	} else {
	    set newval [expr {$p(${which}-abs) + $dir}]
	    catch {uvc parameters $dev ${which}-abs $newval}
	}
    } elseif {[info exists p($which)]} {
	if {[info exists p(${which}-step)]} {
	    if {$index ne {}} {
		set step [lindex [split $p(${which}-step) ,] $index]
	    } else {
		set step $p(${which}-step)
	    }
	    set dir [expr {$dir * $step}]
	}
	if {$index ne {}} {
	    set vals [split $p($which) ,]
	    set newval [expr {[lindex $vals $index] + $dir}]
	    set vals [lreplace $vals $index $index $newval]
	    catch {uvc parameters $dev $which [join $vals ,]}
	} else {
	    set newval [expr {$p($which) + $dir}]
	    catch {uvc parameters $dev $which $newval}
	}
    }
}

# user interface
button .b -command [list startstop .b]
label .l -image [image create photo]
pack .b .l -side top

bind .l <1> {changemirror %W}
bind .l <3> {changeorientation %W}

bind . <Key-plus>  {changeparam zoom 1}
bind . <Key-minus> {changeparam zoom -1}
bind . <Key-Up>    {changeparam pantilt 1 1}
bind . <Key-Down>  {changeparam pantilt -1 1}
bind . <Key-Right> {changeparam pantilt 1 0}
bind . <Key-Left>  {changeparam pantilt -1 0}
bind . <Key-Prior> {changeparam focus-abs 1}
bind . <Key-Next>  {changeparam focus-abs -1}
bind . <Key-g>     {changeparam gain 1}
bind . <Shift-G>   {changeparam gain -1}
bind . <Key-s>     {changeparam saturation 1}
bind . <Shift-S>   {changeparam saturation -1}
bind . <Key-c>     {changeparam contrast 1}
bind . <Shift-C>   {changeparam contrast -1}
bind . <Key-b>     {changeparam brightness 1}
bind . <Shift-B>   {changeparam brightness -1}
bind . <Key-x>     {changeparam sharpness 1}
bind . <Shift-X>   {changeparam sharpness -1}
bind . <Key-f>     {changeparam focus-auto 1}
bind . <Shift-F>   {changeparam focus-auto -1}

# watch for (un)plugged devices
uvc listen [list devicecb .b [.l cget -image]]
# try to init device
init .b [.l cget -image]
