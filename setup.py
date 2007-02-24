import sys

sys.path.insert(-1, '/usr/share/pygtk/2.0')

from distutils.core import setup
from dsextras import PkgConfigExtension, BuildExt

# woah, what a hack
setup(name='vnc',
      version='1.0',
      maintainer='Anthony Liguori',
      maintainer_email='anthony@codemonkey.ws',
      url='http://hg.codemonkey.ws/gtk-vnc',
      description='GTK VNC widget',
      ext_modules=[PkgConfigExtension(name='vnc', pkc_name=('gtk+-2.0', 'gnutls'),
                                      pkc_version='2.0',
                                      sources=['src/continuation.c',
                                               'src/d3des.c',
                                               'src/coroutine.c',
                                               'src/gvnc.c',
                                               'src/vncdisplay.c',
                                               'src/vncmodule.c',
                                               'src/vncshmimage.c',
                                               'src/gen-vnc.defs.c'])])
      
