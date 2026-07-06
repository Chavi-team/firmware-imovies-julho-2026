#!/usr/bin/env python3
"""Gera o ícone do app "Bancada de Fechaduras FI" (Chavi).

Produz:
  packaging/icon-master.png   (1024x1024, master)
  packaging/icon-preview.png  (cópia do master p/ conferência)
  packaging/icon.iconset/*    (tamanhos p/ iconutil -> .icns)
  packaging/icon.icns         (via iconutil, chamado pelo shell depois)
  packaging/icon.ico          (multi-tamanho, via Pillow)

Identidade Chavi: degradê diagonal #B8501F -> #E86628 -> #E12E1D,
squircle (superelipse) com respiro, ícone branco da Fechadura Inteligente FI centralizado.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
BRAND = os.path.expanduser("~/Sites/chavi.com.br/identidade-visual")
FI_WHITE = os.path.join(BRAND, "Ícones/Ícones produtos/PNG FI/Cópia de Fechadura Inteligente White PNG.png")

SIZE = 1024
SS = 4  # supersampling factor para antialias
W = SIZE * SS

# cores da marca
C1 = (0xB8, 0x50, 0x1F)  # laranja escuro
C2 = (0xE8, 0x66, 0x28)  # laranja
C3 = (0xE1, 0x2E, 0x1D)  # flame/vermelho


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def gradient_color(t):
    """Degradê de 3 paradas: 0->C1, 0.5->C2, 1->C3."""
    if t <= 0.5:
        return lerp(C1, C2, t / 0.5)
    return lerp(C2, C3, (t - 0.5) / 0.5)


def make_gradient(w):
    """Degradê diagonal (canto superior-esquerdo -> inferior-direito)."""
    grad = Image.new("RGB", (w, w))
    px = grad.load()
    for y in range(w):
        for x in range(w):
            t = (x + y) / (2 * (w - 1))
            px[x, y] = gradient_color(t)
    return grad


def make_gradient_fast(w):
    """Versão rápida: gera um degradê 1D e mapeia pela diagonal via numpy-free trick.

    Constrói uma barra vertical do degradê ao longo da diagonal e depois
    projeta. Para evitar dependência de numpy, montamos linha a linha usando
    uma LUT de 2*w-1 posições.
    """
    n = 2 * (w - 1) + 1
    lut = [gradient_color(i / (n - 1)) for i in range(n)]
    grad = Image.new("RGB", (w, w))
    px = grad.load()
    for y in range(w):
        base = y
        for x in range(w):
            px[x, y] = lut[base + x]
    return grad


def superellipse_mask(w, n=5.0, inset_ratio=0.0):
    """Máscara de squircle (superelipse) preenchida, com antialias por supersampling
    já embutido no chamador (w é grande)."""
    mask = Image.new("L", (w, w), 0)
    d = ImageDraw.Draw(mask)
    cx = cy = (w - 1) / 2.0
    inset = inset_ratio * w
    a = (w / 2.0) - inset
    # desenha por scanlines resolvendo x da superelipse |x/a|^n + |y/a|^n = 1
    for y in range(w):
        dy = abs((y - cy) / a)
        if dy >= 1.0:
            continue
        dx = (1.0 - dy ** n) ** (1.0 / n)
        x0 = int(round(cx - dx * a))
        x1 = int(round(cx + dx * a))
        d.line([(x0, y), (x1, y)], fill=255)
    return mask


def main():
    print("Gerando degradê...")
    grad = make_gradient_fast(W)

    print("Squircle mask...")
    # squircle com pequeno respiro nas bordas (o próprio PNG tem a forma)
    mask = superellipse_mask(W, n=5.0, inset_ratio=0.012)

    # vinheta interna sutil: escurece levemente os cantos/bordas
    print("Vinheta...")
    vign = Image.new("L", (W, W), 0)
    vd = ImageDraw.Draw(vign)
    cx = cy = (W - 1) / 2.0
    # gradiente radial claro no centro -> escuro nas bordas, bem sutil
    maxr = math.hypot(cx, cy)
    step = 24
    for i in range(step, 0, -1):
        r = maxr * i / step
        # alpha cresce em direção à borda; mantém sutil (máx ~46)
        alpha = int(46 * (i / step) ** 2)
        vd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=alpha)
    vign = vign.filter(ImageFilter.GaussianBlur(W // 60))

    base = grad.convert("RGBA")
    # aplica vinheta (multiplica preto translúcido)
    dark = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    dark.putalpha(vign)
    base = Image.alpha_composite(base, dark)

    # brilho superior sutil (highlight diagonal no topo-esquerdo)
    hl = Image.new("L", (W, W), 0)
    hd = ImageDraw.Draw(hl)
    for i in range(step, 0, -1):
        r = maxr * i / step
        alpha = int(30 * (i / step) ** 2)
        hd.ellipse([cx - r - W * 0.28, cy - r - W * 0.28,
                    cx + r - W * 0.28, cy + r - W * 0.28], fill=alpha)
    hl = hl.filter(ImageFilter.GaussianBlur(W // 40))
    light = Image.new("RGBA", (W, W), (255, 255, 255, 0))
    light.putalpha(hl)
    base = Image.alpha_composite(base, light)

    # compõe o ícone FI branco centralizado
    print("Compondo logo FI...")
    fi = Image.open(FI_WHITE).convert("RGBA")
    # O PNG de origem tem, de cima p/ baixo: o símbolo da fechadura (card) e as
    # palavras "fechadura" / "inteligente". Num ícone pequeno o texto vira
    # borrão e some no laranja -> ficamos SÓ com o símbolo (a fechadura),
    # que é temático (bancada de fechaduras) e legível em qualquer tamanho.
    sym_bbox = fi.crop((0, 0, fi.width, 333)).split()[3].getbbox()
    fi = fi.crop((sym_bbox[0], sym_bbox[1], sym_bbox[2], sym_bbox[3]))
    # respiro de ~18% em cada borda -> área útil ~64% do lado
    avail = int(W * 0.56)
    fw, fh = fi.size
    scale = min(avail / fw, avail / fh)
    nw, nh = int(round(fw * scale)), int(round(fh * scale))
    fi_r = fi.resize((nw, nh), Image.LANCZOS)

    # sombra sutil sob a logo p/ dar leitura
    shadow = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    sh_alpha = fi_r.split()[3].point(lambda a: int(a * 0.35))
    sh_layer = Image.new("RGBA", (nw, nh), (0, 0, 0, 255))
    sh_layer.putalpha(sh_alpha)
    off = int(W * 0.008)
    sx = (W - nw) // 2 + off
    sy = (W - nh) // 2 + off
    shadow.paste(sh_layer, (sx, sy), sh_layer)
    shadow = shadow.filter(ImageFilter.GaussianBlur(W // 90))
    base = Image.alpha_composite(base, shadow)

    logo_layer = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    lx = (W - nw) // 2
    ly = (W - nh) // 2
    logo_layer.paste(fi_r, (lx, ly), fi_r)
    base = Image.alpha_composite(base, logo_layer)

    # aplica a máscara squircle (recorta a forma)
    out = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    out.paste(base, (0, 0), mask)

    # downsample p/ 1024 com antialias
    print("Downsampling p/ 1024...")
    master = out.resize((SIZE, SIZE), Image.LANCZOS)

    master_path = os.path.join(HERE, "icon-master.png")
    preview_path = os.path.join(HERE, "icon-preview.png")
    ico_path = os.path.join(HERE, "icon.ico")
    master.save(master_path)
    master.save(preview_path)
    print("master:", master_path)

    # .ico multi-tamanho
    master.save(ico_path, format="ICO",
                sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print("ico:", ico_path)


if __name__ == "__main__":
    main()
