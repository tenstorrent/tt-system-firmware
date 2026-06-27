# Copyright (c) 2025 Tenstorrent AI ULC
# SPDX-License-Identifier: Apache-2.0

import os
import sys

from pathlib import Path

TTZP = Path(__file__).parent.parent
ZEPHYR_BASE = TTZP.parent / "zephyr"

sys.path.insert(0, str(ZEPHYR_BASE / "doc" / "_extensions"))
sys.path.insert(0, str(ZEPHYR_BASE / "doc" / "_scripts"))
sys.path.insert(0, str(TTZP / "scripts"))

from get_ttzp_version import get_ttzp_version  # noqa: E402

# -- Project information -----------------------------------------------------

project = "TT-System-Firmware"
copyright = "2025, Tenstorrent AI ULC"
author = "Tenstorrent AI ULC"
release = get_ttzp_version(TTZP / "VERSION")

# -- General configuration ---------------------------------------------------

extensions = [
    "myst_parser",
    "sphinx.ext.intersphinx",
    "sphinx_rtd_theme",
    "sphinx_tabs.tabs",
    "zephyr.application",
    "sphinx.ext.graphviz",
    "sphinxcontrib.mermaid",
    "sphinx_sitemap",
]

templates_path = ["_templates"]
exclude_patterns = ["_build_sphinx", "_doxygen/main.md", "Thumbs.db", ".DS_Store"]

source_suffix = {
    ".rst": "restructuredtext",
    ".md": "markdown",
}

# -- Options for HTML output -------------------------------------------------

html_theme = "sphinx_rtd_theme"
html_logo = "images/tt_logo.svg"
html_favicon = "images/favicon.png"
html_static_path = ["_static"]
html_last_updated_fmt = "%b %d, %Y"

reference_prefix = "/tt-system-firmware"
html_context = {
    "project": project,
    "project_code": "tt-system-firmware",
    "reference_links": {"API": f"{reference_prefix}/doxygen/index.html"},
    "current_version": os.environ.get("current_version", release),
}

version = os.environ.get("current_version", release)

external_content_contents = [
    (TTZP / "doc", "[!_]*"),
    (TTZP, "boards/**/*.rst"),
]

intersphinx_mapping = {"zephyr": ("https://docs.zephyrproject.org/latest/", None)}

html_baseurl = "https://tenstorrent.github.io/tt-system-firmware/"
sitemap_locales = [None]
sitemap_url_scheme = "{link}"


def setup(app):
    app.add_css_file("tt_theme.css")
