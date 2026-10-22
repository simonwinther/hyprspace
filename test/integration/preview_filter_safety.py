#!/usr/bin/env python3
"""Targeted real output checks for native preview shader boundaries."""
import json
import time
from PIL import Image
from run import Suite, wait_for
from zoom import inspection, released, wheel

CLIENT = '''import gi, cairo
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk
w=Gtk.Window(title="hs-filter-rgba")
w.set_decorated(False)
w.set_app_paintable(True)
w.set_visual(w.get_screen().get_rgba_visual())
w.set_default_size(640,400)
def draw(widget,c):
    width,height=widget.get_allocated_width(),widget.get_allocated_height()
    c.set_operator(cairo.OPERATOR_SOURCE)
    c.set_source_rgba(0,0,0,0);c.paint()
    for x,y,col in [(0,0,(1,.1,.1)),(1,0,(.1,1,.1)),(0,1,(.1,.1,1)),(1,1,(1,1,.1))]:
        c.set_source_rgba(*col,.55)
        c.rectangle(12+x*(width-24)/2,12+y*(height-24)/2,(width-24)/2,(height-24)/2);c.fill()
    return True
w.connect("draw",draw)
w.connect("destroy",lambda _:Gtk.main_quit())
w.show_all();Gtk.main()
'''
s = Suite(group='preview-filter-safety')
report={'generation':s.snapshot.record,'cases':[],'runtime':str(s.root)}
try:
    client=s.root/'rgba-client.py';client.write_text(CLIENT)
    for p in s.processes[1:]:
        if p.poll() is None:p.terminate();p.wait(timeout=5)
    s.processes=s.processes[:1]
    wait_for(lambda:not s.windows())
    s.ctl('keyword','animations:enabled','false')
    s.ctl('keyword','decoration:shadow:enabled','false')
    s.ctl('keyword','windowrule','match:title ^hs-filter-rgba$, float on')
    name=s.names[0]
    for label,transform,scale,size in [('ordinary',0,1,(640,400)),('portrait-fractional',1,1.25,(360,600))]:
        s.close()
        for p in s.processes[1:]:
            if p.poll() is None:p.terminate();p.wait(timeout=5)
        s.processes=s.processes[:1]
        wait_for(lambda:not s.windows())
        s.ctl('keyword','monitor',f'{name},960x600@60,0x0,{scale},transform,{transform}')
        wait_for(lambda:(m:=next(m for m in s.data('monitors') if m['name']==name))['transform']==transform and abs(m['scale']-scale)<.001)
        s.ctl('keyword','workspace',f'11,monitor:{name},persistent:true')
        s.ctl('keyword','workspace',f'14,monitor:{name},persistent:true')
        s.ctl('dispatch','focusmonitor',name);s.ctl('dispatch','workspace','11')
        proc=s.spawn(['python3',str(client)])
        win=wait_for(lambda:s.windows().get('hs-filter-rgba'))
        s.ctl('dispatch','setfloating','address:'+win['address'])
        s.ctl('dispatch','resizewindowpixel',f'exact {size[0]} {size[1]},address:'+win['address'])
        wait_for(lambda:s.windows()['hs-filter-rgba']['size']==list(size))
        mon=next(m for m in s.data('monitors') if m['name']==name)
        logical_width=mon['width']/mon['scale'];logical_height=mon['height']/mon['scale']
        if transform%2:logical_width,logical_height=logical_height,logical_width
        s.ctl('dispatch','movewindowpixel',f'exact 30 50,address:'+win['address'])
        geometry=s.geometry()
        for blur in ('off','live','cached'):
            s.ctl('keyword','decoration:blur:enabled',str(blur!='off').lower())
            s.ctl('keyword','decoration:blur:new_optimizations',str(blur=='cached').lower())
            s.ctl('keyword','decoration:blur:xray',str(blur=='cached').lower())
            images={};states={};samples={}
            for mode in ('linear','sharp'):
                s.close();s.ctl('keyword','plugin:hyprspace:overview:preview_filter',mode)
                s.ctl('dispatch','hyprspace:overview','on')
                wait_for(lambda:s.status()['resources']['captures']['bytes']>0)
                if mode=='sharp':wait_for(lambda:s.status()['resources']['preview_filter']['bytes']>0)
                time.sleep(.15)
                state=s.status();states[mode]=state
                assert state['resources']['preview_filter']['failures']==0,state['resources']
                path=s.root/f'{label}-{blur}-{mode}.png'
                s.run('grim','-s',str(scale),'-o',name,str(path))
                image=Image.open(path).convert('RGB');images[mode]=image
                tile=s.preview('hs-filter-rgba')
                pixels=[]
                for fx,fy in ((.25,.25),(.75,.25),(.25,.75),(.75,.75)):
                    x=round((tile['x']-mon['x']+tile['w']*fx)*scale)
                    y=round((tile['y']-mon['y']+tile['h']*fy)*scale)
                    pixels.append(image.getpixel((x,y)))
                samples[mode]=pixels
                assert s.geometry()==geometry
                s.close()
                clean=s.status()['resources']
                assert clean['captures']['bytes']==clean['textures']['bytes']==clean['preview_filter']['bytes']==clean['preview_filter']['entries']==0,clean
            differences=[max(abs(a-b) for a,b in zip(first,second)) for first,second in zip(samples['linear'],samples['sharp'])]
            assert max(differences)<=3,(label,blur,samples,differences)
            # The RGBA source's quadrant colours must stay upright. For a
            # transparent source all channels would otherwise match the plate.
            red,green,blue,yellow=samples['sharp']
            assert red[0]>red[1]+20 and green[1]>green[0]+20 and blue[2]>blue[0]+20 and min(yellow[:2])>yellow[2]+20,samples
            case={'label':label,'transform':transform,'scale':scale,'blur':blur,'samples':samples,'max_channel_difference':max(differences),'filter':states['sharp']['resources']['preview_filter']}
            report['cases'].append(case)
            (s.root/'filter-safety.json').write_text(json.dumps(report,indent=2))
            s.check(f'{label}, {blur} blur: native flat RGBA colours agree with filtered draw within 3/255; orientation, input geometry and zero cleanup preserved')
    # One live window makes the filter pass count an unambiguous full-size
    # bypass check. Empty persistent siblings still make the base grid small.
    s.ctl('keyword','decoration:blur:enabled','false')
    s.ctl('keyword','plugin:hyprspace:overview:preview_filter','sharp')
    s.ctl('dispatch','hyprspace:overview','on')
    wait_for(lambda:s.status()['resources']['preview_filter']['bytes']>0)
    s.move(s.preview_point('hs-filter-rgba',.5,.5))
    s.key(44,1)
    inspection(s,wait_for,1)
    wheel(s,-5);inspection(s,wait_for,1.15**5)
    win=s.windows()['hs-filter-rgba'];tile=s.preview('hs-filter-rgba')
    assert tile['w']*scale>=win['size'][0]*scale*.95 and tile['h']*scale>=win['size'][1]*scale*.95,(win,tile)
    time.sleep(.1)
    before=s.status()['resources']['preview_filter'].copy();time.sleep(.25);after=s.status()['resources']['preview_filter'].copy()
    assert after['passes']==before['passes'] and after['allocations']==before['allocations'],(before,after)
    s.run('grim','-s',str(scale),'-o',name,str(s.root/'enlarged-source-bypass.png'))
    s.key(44,0);released(s,wait_for)
    wait_for(lambda:s.status()['resources']['preview_filter']['passes']>after['passes'])
    report['full_size_bypass']={'before':before,'after':after}
    s.close();assert s.status()['resources']['preview_filter']['bytes']==0
    s.check('enlarged stable preview bypasses resampling and allocations; releasing zoom resumes normal grid filtering')
    s.ctl('dispatch','hyprspace:overview','on')
    wait_for(lambda:s.status()['resources']['preview_filter']['bytes']>0)
    s.ctl('plugin','unload',str(s.plugin));s.ctl('plugin','load',str(s.plugin));s.ctl('reload')
    wait_for(lambda:not s.status()['live'])
    assert s.status()['resources']['preview_filter']['entries']==s.status()['resources']['preview_filter']['bytes']==0
    assert not s.status()['keyboard_owned'] and not s.status()['cursor_owned']
    assert s.ctl('configerrors')==''
    s.check('unloading with populated filter buffers and reloading has zero filters and released input ownership')
except BaseException as exc:
    report['error']=repr(exc)
    raise
finally:
    (s.root/'filter-safety.json').write_text(json.dumps(report,indent=2))
    s.finish()
