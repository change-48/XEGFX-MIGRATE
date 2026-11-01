import Cocoa
import QuartzCore

// A-25/A-26 animation surface generator: 120 CALayers, each running
// position / rotation / scale CABasicAnimations forever. CA composes these
// in the render server (WindowServer), so the sample shows who composites.

let app = NSApplication.shared
app.setActivationPolicy(.accessory)

let rect = NSRect(x: 200, y: 200, width: 1100, height: 700)
let win = NSWindow(contentRect: rect,
                   styleMask: [.borderless],
                   backing: .buffered,
                   defer: false)
win.isOpaque = true
win.backgroundColor = .black
win.level = .floating
win.ignoresMouseEvents = true

let host = NSView(frame: rect)
host.wantsLayer = true
host.layer = CALayer()
host.layer?.backgroundColor = NSColor.black.cgColor
host.layer?.masksToBounds = true
win.contentView = host

let root = host.layer!

for i in 0..<120 {
    let l = CALayer()
    let w = CGFloat(40 + (i * 7) % 90)
    let h = CGFloat(30 + (i * 11) % 70)
    l.bounds = CGRect(x: 0, y: 0, width: w, height: h)
    l.position = CGPoint(x: CGFloat(60 + (i * 53) % 1000), y: CGFloat(60 + (i * 37) % 600))
    l.backgroundColor = NSColor(hue: CGFloat(i) / 120.0,
                                saturation: 0.9,
                                brightness: 0.95,
                                alpha: 0.85).cgColor
    l.cornerRadius = CGFloat(i % 12)

    let p = CABasicAnimation(keyPath: "position")
    p.fromValue = NSValue(point: CGPoint(x: 60, y: 60))
    p.toValue = NSValue(point: CGPoint(x: 1040, y: 640))
    p.duration = Double(2 + i % 5)
    p.autoreverses = true
    p.repeatCount = .infinity
    l.add(p, forKey: "pos")

    let r = CABasicAnimation(keyPath: "transform.rotation.z")
    r.fromValue = 0.0
    r.toValue = Double.pi * 2
    r.duration = Double(1 + i % 4)
    r.repeatCount = .infinity
    l.add(r, forKey: "rot")

    let s = CABasicAnimation(keyPath: "transform.scale")
    s.fromValue = 0.6
    s.toValue = 1.6
    s.duration = Double(1 + (i % 3))
    s.autoreverses = true
    s.repeatCount = .infinity
    l.add(s, forKey: "scale")

    root.addSublayer(l)
}

win.orderFrontRegardless()
NSLog("xegfx-anim: 120 layers running")
app.run()
