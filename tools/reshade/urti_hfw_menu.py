"""Horizon Forbidden West : position de « Continuer » dans le menu principal, lue sur une capture de la fenêtre.

    python urti_hfw_menu.py <capture.png>     ->  affiche y (fraction de la hauteur), ou rien si pas de menu principal

Le menu est une colonne de lignes de texte clair à gauche (x de 5 à 20 % de la largeur), espacées de 4,45 % de la
hauteur et ancrées en bas (« Quitter » vers 86,7 %). Une ligne PlayStation Network s'ajoute quand le compte est lié :
la première ligne (« Nouvelle partie ») passe alors de 60 % à 55,6 % et « Continuer », la deuxième, de 64,5 % à 60 %.
En HDR, la capture est délavée (le fond presque aussi clair que le texte) : les lignes sont cherchées à plusieurs seuils.
L'écran des sauvegardes (lignes de texte à gauche aussi) est écarté : un clic y chargerait une ancienne partie.
"""
import sys

import numpy as np
from PIL import Image


def rows(col, h, thr):
    on = (col > thr).mean(axis=1) > 0.04
    out, start = [], None
    for y in range(int(h * 0.4), h):
        if on[y] and start is None:
            start = y
        elif not on[y] and start is not None:
            if y - start >= h * 0.02:  # hauteur d'une ligne de texte (le soulignement de la ligne choisie est écarté)
                out.append((start + y) / 2 / h)
            start = None
    return out


def main():
    a = np.asarray(Image.open(sys.argv[1]).convert('L')).astype(np.float32)
    h, w = a.shape
    col = a[:, int(w * 0.05):int(w * 0.20)]
    found = [y for thr in (200, 235, 250) for y in rows(col, h, thr)]

    def near(target):
        return any(abs(y - target) < 0.012 for y in found)

    # menu principal : « Quitter », « Bonus » et « Paramètres » à leur place
    if not (near(0.867) and near(0.822) and near(0.733)):
        return
    print('0.600' if near(0.556) else '0.645')


if __name__ == '__main__':
    main()
