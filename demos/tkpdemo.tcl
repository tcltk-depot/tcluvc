package require Tk
package require tkpath
package require tcluvc

if {[info command "sdltk"] eq "sdltk"} {
    wm attributes . -fullscreen 1
}

pack [tkp::canvas .c -bg "#c6ceef" -highlightthickness 0] -fill both -expand 1

set dev [lindex [uvc devices] 0]
set dev [uvc open $dev getimage]

proc getminframesize dev {
    set index -1
    foreach {i d} [uvc listformats $dev] {
	array set f $d
	lassign [split $f(frame-size) x] width height
	if {![info exists minwidth] || ($width < $minwidth)} {
	    set minwidth $width
	    set minheight $height
	    set index $i
	}
    }
    return [list $index $minwidth $minheight]
}

lassign [getminframesize $dev] index width height
uvc format $dev $index

set x0 [expr {1.5 * $width - 20}]
set y0 $x0
set dx [expr {$width * 1.3}]
set dy $dx

set anchors [list nw n ne w c e sw s se]
for {set i 0} {$i < [llength $anchors] } {incr i} {
    set a [lindex $anchors $i]
    set x [expr {$x0 + $i%3 * $dx}]
    set y [expr {$x0 + $i/3 * $dy}]
    set t $a
    image create photo img$i
    img$i configure -width $width -height $height
    .c create pimage $x $y -image img$i -anchor $a -tags $a
    .c create ptext $x $y -text $t -fontfamily "Times" \
	-fontsize 23 -fill black -stroke white -strokewidth 3 \
	-filloverstroke 1 -textanchor middle
    .c create path "M $x $y m -5 0 h 10 m -5 -5 v 10" -stroke red
}

proc getimage dev {
    incr ::count
    set ::count [expr {$::count % [llength $::anchors]}]
    uvc image $dev img$::count
    if {$::count != 4} {
	# full rate in center image
        uvc image $dev img4
    }
}

proc ticker {deg step tim} {
    if {[winfo exists .c]} {
	after $tim [list ticker [expr {[incr deg $step] % 360}] $step $tim]
	set phi [expr 2*$deg*3.14159/360.0]
	for {set i 0} {$i < [llength $::anchors]} {incr i} {
	    set a [lindex $::anchors $i]
	    set x [expr {$::x0 + $i%3 * $::dx}]
	    set y [expr {$::y0 + $i/3 * $::dy}]
	    set m [::tkp::matrix rotate $phi $x $y]
	    .c itemconfigure $a -m $m
	}
    }
}

bind all <Key-q> exit
bind all <Escape> exit

set count 0
ticker 0 1 100
uvc start $dev
