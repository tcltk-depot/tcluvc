# Anaglyph 3D demo using tcluvc
#
# A dual UVC camera is required, the init_cameras
# procedure below must be adapted for the proper
# USB vendor/product IDs of the UVC camera devices.
#
# Added bonus: on some acquired images a zbar
# recognizer is applied which overlays detected
# QR code data with the image.

package require Tk
package require tcluvc
package require zbar

catch {
    # if running in undroidwish ...
    sdltk root 500 450
    wm attributes . -fullscreen 1
    # if running in AndroWish ...
    catch {
	borg screenorientation landscape
	sdltk touchtranslate 0
    }
}

canvas .img -width 500 -height 450 -borderwidth 0 -highlightthickness 4 \
   -highlightcolor black -background blue -highlightbackground black
pack .img -side left

image create photo L
image create photo R

.img create image 250 225 -image L -anchor c

bind .img  <1> flip_images
bind all <Escape> exit
bind all <Break>  exit

# button press: flip images L <-> R
proc flip_images {} {
    foreach dev [array names ::imgs] {
	if {$::imgs($dev) eq "L"} {
	    set ::imgs($dev) "R"
	} else {
	    set ::imgs($dev) "L"
	}
    }
}

# callback: image ready or error
proc image_callback {dev} {
    set iname $::imgs($dev)
    uvc image $dev $iname
    incr ::icnt -1
    if {$::icnt <= 0} {
	set ::icnt 7
	catch {zbar::async_decode $::imgs($dev) zbar_done}
    }
    # both images available, combine to anaglyph
    uvc mcopy L R 0x0000ffff
}

# callback: zbar recognizer
proc zbar_done {time type data} {
    if {$type ne ""} {
	set data [encoding convertfrom identity $data]
	show_data $data
	set ::icnt 50
    }
}

# display decoded QR code(s)
proc show_data {data} {
    after cancel [list show_data {}]
    .img delete zbar_data
    if {$data eq ""} {
	return
    }
    regsub -all {[[:cntrl:]]} $data " " data
    set prdata ""
    while {[string length $data]} {
	append prdata [string range $data 0 59] "\n"
	set data [string range $data 60 end]
    }
    .img create text 250 300 -text $prdata -tags zbar_data -fill black \
	-font {Helvetica -14} -anchor c -justify left
    .img create text 251 301 -text $prdata -tags zbar_data -fill white \
	-font {Helvetica -14} -anchor c -justify left
    after 2000 [list show_data {}]
}

# initialize both cameras
proc init_cameras {} {
    set ::icnt 10
    set imglist {L R}
    set count 0
    # AndroWish specific: fetch list of usb devices
    catch {
	foreach {d n} [borg usbdevices] {
	    set nmap($n) $d
	}
    }
    foreach {devid vendor prod} [uvc devices] {
	# lsusb is our friend for the vendor/product IDs here
	if {[string match -nocase {0ac8:990[12]:*} $devid]} {
	    # AndroWish specific: get permission for device
	    set n [string tolower [join [lrange [split $devid ":"] 0 1] ":"]]
	    if {[info exists nmap($n)]} {
		borg usbpermission $nmap($n) 1
	    }
	    set img [lindex $imglist $count]
	    set dev [uvc open $devid image_callback]
	    foreach {index fmt} [uvc listformats $dev] {
		if {[dict get $fmt frame-size] eq "640x480" &&
		    [dict get $fmt mjpeg]} {
		    uvc format $dev $index 10
		    $img configure -width 640 -height 480
		}
	    }
	    set ::imgs($dev) $img
	    set ::devs($img) $dev
	    incr count
	    if {$count > 1} {
		break
	    }
	}
    }
    if {$count != 2} {
	error "need two cameras"
    }
    foreach img [array names ::devs] {
	uvc start $::devs($img)
    }
}

init_cameras
