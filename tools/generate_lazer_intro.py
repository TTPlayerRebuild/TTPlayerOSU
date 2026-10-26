"""Generate shader DATA maps for the embedded TTPlayer badge, not new artwork.

Development-only tool: Python + Pillow. The checked-in RGB PNGs are embedded by
the normal build, which has no Pillow dependency. R is reveal time, G is stroke
coverage, B is zero; PNG alpha must remain opaque so decoding cannot premultiply
the data channels. The original TTPlayer.png is opened read-only.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
from PIL import Image, ImageChops, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SHA256 = 'ebe8e9d93bdc59ed28a429f4bf970622376d4dd81e16ea54fe9094a9f9163d28'


def pixels_of(image):
    # Pillow 14 replaces getdata with get_flattened_data; keep older developer
    # installations usable too.
    return image.get_flattened_data() if hasattr(image, 'get_flattened_data') else image.getdata()


def cubic_path(start, commands):
    """Sample calibrated cubic curves at <= 0.5px control-polygon spacing.

    These describe the badge's geometry, not the edges of its metallic glare.
    In particular, the headband stays smooth across the source's bright glints.
    """
    points = [start]
    for command in commands:
        p0 = points[-1]
        if command[0] == 'L':
            end = command[1:]
            count = max(1, math.ceil(math.dist(p0,end)*2))
            points.extend(tuple(a+(b-a)*i/count for a,b in zip(p0,end)) for i in range(1,count+1))
        else:
            p1,p2,p3 = command[1:3],command[3:5],command[5:7]
            count = max(1, math.ceil(sum(math.dist(a,b) for a,b in zip((p0,p1,p2),(p1,p2,p3)))*2))
            for i in range(1,count+1):
                t,u = i/count,1-i/count
                points.append(tuple(u*u*u*a+3*u*u*t*b+3*u*t*t*c+t*t*t*d for a,b,c,d in zip(p0,p1,p2,p3)))
    if math.dist(points[-1],start) > .001:
        raise ValueError('Open calibrated contour')
    points.pop()
    first = min(range(len(points)),key=lambda i:(points[i][1],points[i][0]))
    return points[first:]+points[:first]


def extract_paths(source: Image.Image):
    width, height = source.size
    if width != height or width != 1080:
        raise ValueError('This feature mask is calibrated for the 1080px TTPlayer badge')
    pixels = list(pixels_of(source))
    alpha = source.getchannel('A')
    silhouette = alpha.point(lambda a: 255 if a >= 128 else 0)
    box = silhouette.getbbox()
    cx, cy = (box[0]+box[2]-1)/2, (box[1]+box[3]-1)/2
    radius = (box[2]-box[0]+box[3]-box[1])/4
    # Locate the inner orange-band edge in the actual source. Use median radial
    # colour discontinuity so the headphones cannot introduce a false ring.
    radii = []
    for degrees in range(0,360,2):
        angle = math.radians(degrees)
        candidates = []
        for r in range(round(radius*.88),round(radius*.97)):
            samples = []
            for d in (-2,2):
                x = round(cx+(r+d)*math.cos(angle))
                y = round(cy+(r+d)*math.sin(angle))
                samples.append(pixels[y*width+x][:3])
            candidates.append((sum((a-b)**2 for a,b in zip(*samples)),r))
        radii.append(max(candidates)[1])
    inner_radius = statistics.median(radii)
    def circle(r):
        return [(cx+r*math.sin(i*2*math.pi/4096),cy-r*math.cos(i*2*math.pi/4096)) for i in range(4096)]
    # Inset the 4px outer stroke so its entire antialiased width fits inside the
    # supplied silhouette instead of following its resampled alpha stair-steps.
    paths = [('outer-ring',circle(radius-2.5)),('inner-ring',circle(inner_radius))]
    # Coordinates calibrated against the source SHA above: the elliptical
    # headband and rounded cups form one outline. No brightness thresholding,
    # glare islands, self-intersections, or steps at the earcup attachment.
    paths.append(('headphones',cubic_path((544.5,193),[
        ('C',703.8,193,832,329,833,497),
        ('C',833,499,834,500,836,500),('L',870,500),
        ('C',912,500,946,534,946,576),('L',946,709),
        ('C',946,750,913,783,872,783),('L',786,783),
        ('C',785,783,784,782,784,781),('L',784,500),
        ('C',784,352,676.8,232,544.5,232),
        ('C',412.2,232,305,352,305,500),('L',305,781),
        ('C',305,782,304,783,303,783),('L',218,783),
        ('C',176,783,143,750,143,708),('L',143,575),
        ('C',143,533,176,500,218,500),('L',253,500),
        ('C',255,500,256,499,256,497),
        ('C',257,329,385.2,193,544.5,193),
    ])))
    paths.append(('waveform',cubic_path((346,598),[
        ('L',396,598),('C',407,598,414,604,419,615),
        ('L',455,700),('C',456,702,457,702,458,700),
        ('L',529,532),('C',537,511,562,512,572,533),
        ('L',624,660),('C',625,662,626,662,627,660),
        ('L',645,615),('C',649,605,657,600,670,600),('L',739,600),
        ('C',752,600,763,609,763,621),('C',763,634,752,644,738,644),
        ('L',683,644),('C',682,644,681,645,680,647),
        ('L',648,722),('C',640,744,613,745,603,722),
        ('L',551,594),('C',550,592,549,592,548,594),
        ('L',476,765),('C',469,783,443,784,433,761),
        ('L',382,643),('C',381,641,380,641,379,641),('L',346,641),
        ('C',333,641,323,632,323,620),('C',323,608,333,598,346,598),
    ])))
    return paths, alpha, inner_radius


def progress_curve(path: Path):
    # Match the distribution of reveal thresholds in the existing original
    # maps. The path geometry is TTPlayer-specific; no osu! glyph is transferred.
    values = sorted(r for r,g,b in pixels_of(Image.open(path).convert('RGB')) if g>=200)
    return [values[round(i*(len(values)-1)/1024)] for i in range(1025)]


def generate(output: Path):
    source_path = ROOT/'assets/TTPlayer.png'
    if hashlib.sha256(source_path.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError('Source changed; recalibrate the TTPlayer feature extraction first')
    source = Image.open(source_path).convert('RGBA')
    paths, alpha, inner_radius = extract_paths(source)
    curves = [progress_curve(ROOT/f'assets/osu-lazer-intro-{kind}.png') for kind in ('highlight','background')]
    scale = 4
    size = tuple(v*scale for v in source.size)
    coverage = Image.new('L',size)
    draw = ImageDraw.Draw(coverage)
    segments = []
    for name, points in paths:
        points = points+[points[0]]
        draw.line([(x*scale,y*scale) for x,y in points],fill=255,width=4*scale,joint='curve')
        lengths = [math.dist(a,b) for a,b in zip(points,points[1:])]
        total, distance = sum(lengths), 0.
        for a,b,length in zip(points,points[1:],lengths):
            segments.append((distance/total,a,b))
            distance += length
    coverage = coverage.resize(source.size,Image.Resampling.LANCZOS)
    # Never introduce a new opaque outline beyond the supplied PNG's silhouette.
    coverage = ImageChops.multiply(coverage,alpha)
    red = [Image.new('L',size,255),Image.new('L',size,255)]
    drawers = [ImageDraw.Draw(image) for image in red]
    # At contour intersections keep the earliest threshold, not whichever
    # contour happened to be enumerated last. Round R support covers the full
    # antialiasing halo. Short, wide ImageDraw.line segments alone can leave
    # holes at quantized joins, which would turn into permanent black speckles.
    for progress,a,b in sorted(segments,reverse=True):
        index = min(1024,round(progress*1024))
        line = [(a[0]*scale,a[1]*scale),(b[0]*scale,b[1]*scale)]
        for draw,curve in zip(drawers,curves):
            draw.line(line,fill=curve[index],width=8*scale)
            x,y = line[0]
            support = 6*scale
            draw.ellipse((x-support,y-support,x+support,y+support),fill=curve[index])
    output.mkdir(parents=True,exist_ok=True)
    files = {}
    for kind,channel in zip(('highlight','background'),red):
        channel = channel.resize(source.size,Image.Resampling.NEAREST)
        if any(red==255 and green for red,green in zip(channel.tobytes(),coverage.tobytes())):
            raise ValueError('Unassigned reveal threshold within antialiased stroke')
        # Blank pixels are explicit R=255,G=0,B=0; A is absent/opaque.
        channel.paste(255,mask=coverage.point(lambda v: 255 if v==0 else 0))
        image = Image.merge('RGB',(channel,coverage,Image.new('L',source.size)))
        path = output/f'ttplayer-lazer-intro-{kind}.png'
        image.save(path,optimize=True)
        files[path.name] = {'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                           'pixel_sha256':hashlib.sha256(image.tobytes()).hexdigest()}
    manifest = {'source':'TTPlayer.png','source_sha256':SOURCE_SHA256,'size':list(source.size),
                'channels':{'R':'strict progress threshold','G':'stroke alpha','B':0,'A':255},
                'inner_ring_radius':inner_radius,'stroke_width':4,'supersample':scale,
                'geometry':'calibrated cubic Bezier headphones/waveform and analytic rings',
                'contours':[{'feature':name,'points':len(points)} for name,points in paths],
                'files':files}
    (output/'ttplayer-lazer-intro.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(manifest,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,default=ROOT/'assets')
    generate(parser.parse_args().output)
