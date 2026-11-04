"""Horizon Forbidden West : position de « Continuer » dans le menu principal, lue sur une capture de la fenêtre.

    python urti_hfw_menu.py <capture.png>     ->  affiche y (fraction de la hauteur), ou rien si pas de menu

Le menu est une colonne de lignes de texte clair à gauche (x de 5 à 20 % de la largeur), ancrée en bas : le nombre
de lignes change (une ligne PlayStation Network s'ajoute quand le compte est lié), « Continuer » est la deuxième.
"""
import sys

import numpy as np
from PIL import Image


def main():
    a = np.asarray(Image.open(sys.argv[1]).convert('L')).astype(np.float32)
    h, w = a.shape
    col = a[:, int(w * 0.05):int(w * 0.20)]
    # lignes de pixels avec beaucoup de texte clair (lettres blanches ou dorées) dans la colonne du menu
    bright = (col > 200).mean(axis=1)
    rows = bright > 0.04
    runs, start = [], None
    for y in range(int(h * 0.4), h):
        if rows[y] and start is None:
            start = y
        elif not rows[y] and start is not None:
            if y - start >= h * 0.02:  # hauteur d'une ligne de texte (le soulignement de la ligne choisie, plus fin, est écarté)
                runs.append((start, y))
            start = None
    if len(runs) < 4:
        return
    print(f'{(runs[1][0] + runs[1][1]) / 2 / h:.3f}')


if __name__ == '__main__':
    main()
