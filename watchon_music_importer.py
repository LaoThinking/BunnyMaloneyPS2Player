#!/usr/bin/env python3
"""WATCHON TRAX music importer for the PS2 host: filesystem.

Usage (inside ~/BunnyMaloneyWatchon):
 python3 watchon_music_importer.py "/mnt/c/Users/.../Music/My Song.mp3"
 python3 watchon_music_importer.py "/mnt/c/Users/.../Music"
 python3 watchon_music_importer.py --list

The original music files are never modified. FFmpeg + Pillow required.
"""
import argparse
import json
import os
from pathlib import Path
import math
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata

from PIL import Image, ImageDraw, ImageFont, ImageOps

FORMATS={'.mp3','.flac','.wav','.m4a','.aac','.ogg','.opus','.wma','.aiff','.aif','.ape','.alac','.mp4','.mka','.ac3'}
THEMES=[
    ((67,72,79),(28,31,36),(47,50,58)),
    ((76,79,84),(34,35,39),(56,58,63)),
    ((58,65,72),(24,29,36),(40,45,51)),
    ((72,72,76),(29,29,32),(49,48,53)),
]

def ascii_ps2(s, default, limit=55):
    s=str(s or default).strip()
    replacements={'ł':'l','Ł':'L','ø':'o','Ø':'O','ß':'ss','æ':'ae','Æ':'AE'}
    s=''.join(replacements.get(ch,ch) for ch in s)
    s=unicodedata.normalize('NFKD',s).encode('ascii','ignore').decode('ascii')
    s=re.sub(r'[\t\r\n]+',' ',s)
    s=''.join(c for c in s if 32<=ord(c)<=126)
    return (s.strip() or default)[:limit]

def font_for(project, size, bold=True):
    locations=[project/'Boicot(4).otf',project/'Boicot.otf',
        Path('/mnt/c/Users/BlxxDVST/Downloads/Boicot(4).otf'),
        Path('/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf')]
    for path in locations:
        try:
            return ImageFont.truetype(str(path),size)
        except OSError:
            pass
    return ImageFont.load_default()

def fit_font(project,draw,text,width,initial_size):
    size=initial_size
    while size>=10:
        f=font_for(project,size)
        bb=draw.textbbox((0,0),text,font=f)
        if bb[2]-bb[0]<=width:
            return f
        size-=1
    return font_for(project,10)

def panel_art(project,title,artist,album,variant):
    W,H=320,96
    top,mid,bot=THEMES[variant]
    out=Image.new('RGB',(W,H),(0,0,0)); px=out.load()
    for y in range(H):
        if y<50: t=y/50;a,b=top,mid
        else: t=(y-50)/45;a,b=mid,bot
        base=[round(a[k]*(1-t)+b[k]*t) for k in range(3)]
        dy=y-(H-1)/2
        arc=math.sqrt(max(0,47.75**2-dy**2))
        for x in range(W):
            if x<arc: continue
            if x>=310 and (y<10 or y>=H-10):
                dx=x-309.5
                ddy=(y-9.5 if y<10 else y-(H-10.5))
                if dx*dx+ddy*ddy>100: continue
            c=base.copy()
            if x-arc<2.2: c=[164,172,181] if y<55 else [105,112,122]
            elif x-arc<5.0: c=[87,95,105]
            elif x-arc<8.0: c=[17,20,24]
            if y<3: c=[min(240,int(v*1.65)+24) for v in c]
            elif y<6: c=[min(180,int(v*1.15)+11) for v in c]
            elif y>92: c=[min(160,int(v*1.3)+6) for v in c]
            if 267<=x<=271: c=[130,137,148] if x<=269 else [18,22,29]
            elif 272<=x<=317: c=[20,22,27] if 275<=x<=315 else [92,99,109]
            if x>=316: c=[160,163,167]
            px[x,y]=tuple(c)
    d=ImageDraw.Draw(out)
    d.line(((72,34),(260,34)),fill=(122,129,138),width=1)
    d.line(((72,64),(260,64)),fill=(13,15,19),width=1)
    for txt,y,sz,col in [(title,7,21,(237,240,243)),
                         (artist,37,19,(213,219,225)),
                         (album,68,13,(169,180,188))]:
        f=fit_font(project,d,txt,181,sz)
        d.text((76,y),txt,font=f,fill=col)
    label=Image.new('RGB',(98,32),(20,22,27)); ld=ImageDraw.Draw(label)
    f=fit_font(project,ld,'TRAX',86,27)
    bb=ld.textbbox((0,0),'TRAX',font=f)
    ld.text(((98-(bb[2]-bb[0]))/2-bb[0],(32-(bb[3]-bb[1]))/2-bb[1]),
            'TRAX',font=f,fill=(242,245,248))
    label=label.rotate(90,expand=True)
    out.paste(label,(275+(32-label.width)//2,(H-label.height)//2))
    return out

def disc_frame(cover,frame):
    n=64
    base=ImageOps.fit(cover.convert('RGB'),(n,n),method=Image.Resampling.LANCZOS)
    rotated=base.rotate(-frame*45.0,resample=Image.Resampling.BICUBIC)
    out=Image.new('RGB',(n,n),(0,0,0)); pixels=out.load(); art=rotated.load()
    c=(n-1)/2
    for y in range(n):
        for x in range(n):
            rr=math.hypot(x-c,y-c)
            if rr>31: continue
            if rr>=29.7: rgb=(193,204,214) if x+y<n else (107,114,125)
            elif rr>=28.2: rgb=(28,31,39)
            elif rr>=27.4: rgb=(102,111,122)
            else: rgb=art[x,y]
            if rr<3.4: rgb=(13,15,18)
            elif rr<5.3: rgb=(203,211,218)
            elif rr<6.8: rgb=(43,47,56)
            pixels[x,y]=rgb
    return out

def probe(path):
    proc=subprocess.run(['ffprobe','-v','error','-show_format','-show_streams',
        '-of','json',str(path)],text=True,capture_output=True)
    if proc.returncode:
        raise RuntimeError(proc.stderr.strip() or 'ffprobe failed')
    data=json.loads(proc.stdout)
    format_info=data.get('format',{})
    tags={k.lower():str(v) for k,v in format_info.get('tags',{}).items()}
    for stream in data.get('streams',[]):
        if stream.get('codec_type')=='audio':
            tags.update({k.lower():str(v) for k,v in stream.get('tags',{}).items() if k.lower() not in tags})
            break
    dur=float(format_info.get('duration') or 0)
    return tags,int(max(0,dur*1000))

def cover_image(project,source):
    with tempfile.TemporaryDirectory(prefix='watchon_art_') as td:
        target=Path(td)/'embedded.png'
        cmd=['ffmpeg','-nostdin','-hide_banner','-loglevel','error','-y',
             '-i',str(source),'-map','0:v:0','-frames:v','1',str(target)]
        p=subprocess.run(cmd,capture_output=True,text=True)
        if p.returncode==0 and target.is_file():
            try:
                with Image.open(target) as im:
                    return im.convert('RGB').copy()
            except OSError: pass
    for name in ['cover.jpg','cover.png','folder.jpg','folder.png','front.jpg','front.png']:
        path=source.parent/name
        if path.is_file():
            with Image.open(path) as im: return im.convert('RGB').copy()
    fallback=project/'Music'/'UI'/'fallback_cover.jpg'
    if not fallback.is_file(): raise RuntimeError('Missing WATCHON TRAX default photo: '+str(fallback))
    with Image.open(fallback) as im: return im.convert('RGB').copy()

def atomic_json(path,data):
    temp=path.with_suffix(path.suffix+'.tmp')
    temp.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    temp.replace(path)

def main():
    parser=argparse.ArgumentParser(description='Import audio into Bunny Maloney WATCHON TRAX')
    parser.add_argument('paths',nargs='*',help='Song files or folders (including Windows /mnt/c paths)')
    parser.add_argument('--list',action='store_true',help='Show library without importing')
    parser.add_argument('--project',default='.',help='BunnyMaloneyWatchon directory')
    args=parser.parse_args()
    project=Path(args.project).expanduser().resolve()
    music=project/'Music'
    for sub in ['Audio','Covers','Panels','UI']: (music/sub).mkdir(parents=True,exist_ok=True)
    catalog=music/'library.json'
    data=json.loads(catalog.read_text(encoding='utf-8')) if catalog.exists() else {'version':1,'tracks':[]}
    tracks=data['tracks']
    if args.list:
        print('WATCHON TRAX library:',len(tracks),'tracks')
        for t in tracks: print(t['id'],t['title'],'-',t['artist'])
        return
    sources=[]
    for name in args.paths:
        path=Path(name).expanduser()
        if path.is_file(): sources.append(path)
        elif path.is_dir(): sources.extend(sorted(p for p in path.rglob('*') if p.is_file() and p.suffix.lower() in FORMATS))
        else: print('[SKIP] Missing:',path)
    if not sources:
        print('Example: python3 watchon_music_importer.py "/mnt/c/Users/YourName/Music/Track.mp3"')
        return
    if not shutil.which('ffmpeg') or not shutil.which('ffprobe'):
        raise SystemExit('Install FFmpeg first: sudo apt install ffmpeg')
    good=0
    for source in sources:
        source=source.resolve()
        # Explicitly supplied files are allowed even with unusual extensions;
        # FFprobe/FFmpeg, not the suffix, decide whether audio is supported.
        if any(t.get('source_path')==str(source) for t in tracks):
            print('[SKIP] Already imported:',source.name);continue
        if len(tracks)>=160: print('[STOP] Library reached 160 PS2 tracks');break
        next_id=max([int(x['id']) for x in tracks]+[0])+1
        tid=f'{next_id:04d}'
        print('[IMPORT]',source.name, '=>', tid,flush=True)
        out=music/'Audio'/f'{tid}.pcm'
        temp=out.with_suffix('.pcm.partial')
        try:
            tags,duration=probe(source)
            title=ascii_ps2(tags.get('title'),source.stem)
            artist=ascii_ps2(tags.get('artist') or tags.get('album_artist'), 'Unknown Artist')
            album=ascii_ps2(tags.get('album'),'Unknown Album')
            cover=cover_image(project,source)
            proc=subprocess.run(['ffmpeg','-nostdin','-hide_banner','-loglevel','error','-y',
                '-i',str(source),'-map','0:a:0','-vn','-ac','2','-ar','22050',
                '-c:a','pcm_s16le','-f','s16le',str(temp)],capture_output=True,text=True)
            if proc.returncode or not temp.is_file() or temp.stat().st_size<4:
                raise RuntimeError(proc.stderr.strip() or 'No PCM output')
            temp.replace(out)
            for i in range(4):
                panel_art(project,title,artist,album,i).save(music/'Panels'/f'{tid}_{i}.bmp')
            for i in range(8):
                disc_frame(cover,i).save(music/'Covers'/f'{tid}_{i:02d}.bmp')
            tracks.append(dict(id=tid,title=title,artist=artist,album=album,
                duration_ms=duration,source_path=str(source),source_format=source.suffix.lower()))
            good+=1
            print('[OK]',title,'-',artist, '|', duration//1000,'seconds')
        except Exception as exc:
            print('[FAILED]',source.name,':',exc)
            for p in [temp,out]+list((music/'Panels').glob(f'{tid}_*.bmp'))+list((music/'Covers').glob(f'{tid}_*.bmp')):
                try:p.unlink()
                except FileNotFoundError:pass
    atomic_json(catalog,data)
    output=['# id\ttitle\tartist\talbum\tduration_ms']
    for t in tracks:
        output.append('\t'.join([t['id'],t['title'],t['artist'],t['album'],str(t.get('duration_ms',0))]))
    (music/'tracks.tsv').write_text('\n'.join(output)+'\n',encoding='ascii',errors='replace')
    print('\n[DONE]',good,'new tracks;',len(tracks),'total.')
    print('Restart your PS2 ELF in PCSX2 to read the updated library. No recompilation needed.')

if __name__=='__main__':
    main()
