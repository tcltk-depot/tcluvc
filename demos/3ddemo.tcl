package require Tk
package require Canvas3d
package require tcluvc

if {[info command "sdltk"] eq "sdltk"} {
    wm attributes . -fullscreen 1
}

canvas3d .w1
pack .w1 -expand yes -fill both -side top
.w1 configure -width 300 -height 300 -background black

bind all <Break> exit
bind all <Key-q> exit
bind all <Escape> exit

# Mouse control
bind .w1 <B1-Motion> {
    set ry [expr 360.0 * (%y  - $::Y) / [%W cget -height]]
    set rx [expr 360.0 * (%x  - $::X) / [%W cget -width]]
    %W transform -camera type(light) [list orbitup $ry orbitleft $rx]
    set ::X %x
    set ::Y %y
}

bind .w1 <1> {
    set ::X %x
    set ::Y %y
}

# Mouse wheel for zoom in/out
bind .w1 <5> {%W transform -camera type(light) {movein 0.98}}
bind .w1 <4> {%W transform -camera type(light) {movein 1.02}}

# Rotate around the scene center:
bind .w1 <Up> {%W transform -camera type(light) {orbitup 5.0}}
bind .w1 <Down> {%W transform -camera type(light) {orbitdown 5.0}}
bind .w1 <Left> {%W transform -camera type(light) {orbitright 5.0}}
bind .w1 <Right> {%W transform -camera type(light) {orbitleft 5.0}}

# Zoom in and out:
bind .w1 <Key-s> {%W transform -camera type(light) {movein 0.9}}
bind .w1 <Key-x> {%W transform -camera type(light) {movein 1.1}}

# Rotate camera around line of sight:
bind .w1 <Key-c> {%W transform -camera type(light) {twistright 5.0}}
bind .w1 <Key-z> {%W transform -camera type(light) {twistleft 5.0}}
bind .w1 <Key-y> {%W transform -camera type(light) {twistleft 5.0}}

# Look to the left or right.
bind .w1 <Key-d> {%W transform -camera type(light) {panright 5.0}}
bind .w1 <Key-a> {%W transform -camera type(lignt) {panleft 5.0}}

# Lookat!
bind .w1 <Key-l> {%W transform -camera type(light) {lookat all}}

proc cube {w sidelength tag} {
    set p [expr $sidelength / 2.0]
    set m [expr $sidelength / -2.0]

    $w create polygon [list $p $p $p  $m $p $p  $m $p $m  $p $p $m] -tags x0
    $w create polygon [list $p $m $p  $m $m $p  $m $m $m  $p $m $m] -tags x1

    $w create polygon [list $p $p $p  $p $m $p  $p $m $m  $p $p $m] -tags x2
    $w create polygon [list $m $p $p  $m $m $p  $m $m $m  $m $p $m] -tags x3
}

cube .w1 1.0 cube_one
.w1 create light {0.0 0.0 4.0}
.w1 transform -camera light {lookat all}

proc imagecb {w img dev} {
    incr ::count
    # find..viewport is unusual slow in undroidwish
    # maybe due to glRenderMode(GL_SELECT) running in software
    if {[info command ::sdltk] eq "::sdltk"} {
	uvc image $dev $img
	foreach tag {x0 x1 x2 x3} {
	    $w itemconfigure $tag -teximage {}
	    $w itemconfigure $tag -teximage $img
	}
    } else {
	set found 0
	lassign [winfo pointerxy $w] x y
	set x [expr {$x - [winfo rootx $w]}]
	set y [expr {$y - [winfo rooty $w]}]
	foreach id [$w find -sortbydepth viewport($x,$y)] {
	    set tag [$w gettag $id]
	    if {[string match "x*" $tag]} {
		set found 1
		break
	    }
	}
	if {!$found} {
	    set tag x[expr {$::count % 4}]
	}
	uvc image $dev $img
	$w itemconfigure $tag -teximage {}
	$w itemconfigure $tag -teximage $img
    }
}

proc startstop {dev} {
    if {[uvc state $dev] eq "capture"} {
	uvc stop $dev
    } else {
	uvc start $dev
    }
}

focus .w1
set img [image create photo]
set dev [uvc open [lindex [uvc devices] 0] [list imagecb .w1 $img]]

bind .w1 <Key-space> [list startstop $dev]

set count 0
uvc format $dev 0 10
uvc start $dev

