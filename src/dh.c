/*
 * GTK VNC Widget, Diffie Hellman
 *
 * Copyright (C) 2008  Red Hat, Inc
 *
 * Derived from gnutls_dh.c, also under:
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <config.h>

#include "dh.h"
#include "vncutil.h"

/*
 * General plan, as per gnutls_dh.c
 *
 *
 *  VNC server: X = g ^ x mod p;
 *  VNC client: Y = g ^ y mod p;
 *
 *  Server key = Y ^ x mod p;
 *  Client key = X ^ y mod p;
 *
 * Where
 *   g == gen
 *   p == mod
 *
 *   y == priv
 *   Y == pub
 *  Client key == key
 *
 *
 */

struct vnc_dh {
       gcry_mpi_t gen;  /* g */
       gcry_mpi_t mod;  /* p */

       gcry_mpi_t priv; /* y */
       gcry_mpi_t pub;  /* Y = g ^ y mod p */

       gcry_mpi_t key;  /*     X ^ y mod p */
};

#define VNC_DH_MAX_BITS 31

struct vnc_dh *vnc_dh_new(gcry_mpi_t gen, gcry_mpi_t mod)
{
       struct vnc_dh *ret = g_new0(struct vnc_dh, 1);

       ret->gen = gcry_mpi_copy(gen);
       ret->mod = gcry_mpi_copy(mod);

       return ret;
}


gcry_mpi_t vnc_dh_gen_secret(struct vnc_dh *dh)
{
       if (!(dh->priv = gcry_mpi_new(VNC_DH_MAX_BITS)))
               abort();

       do {
               gcry_mpi_randomize (dh->priv, (VNC_DH_MAX_BITS / 8) * 8, GCRY_STRONG_RANDOM);
       } while (gcry_mpi_cmp_ui (dh->priv, 0) == 0);

       if (!(dh->pub = gcry_mpi_new(VNC_DH_MAX_BITS)))
               abort();

       gcry_mpi_powm(dh->pub, dh->gen, dh->priv, dh->mod);

       return dh->pub;
}

gcry_mpi_t vnc_dh_gen_key(struct vnc_dh *dh, gcry_mpi_t inter)
{
       if (!(dh->key = gcry_mpi_new(VNC_DH_MAX_BITS)))
               abort();

       gcry_mpi_powm(dh->key, inter, dh->priv, dh->mod);

       return dh->key;
}

void vnc_dh_free(struct vnc_dh *dh)
{
       if (dh->key)
               gcry_mpi_release(dh->key);
       if (dh->pub)
               gcry_mpi_release(dh->pub);
       if (dh->priv)
               gcry_mpi_release(dh->priv);
       if (dh->mod)
               gcry_mpi_release(dh->mod);
       if (dh->gen)
               gcry_mpi_release(dh->gen);
       g_free(dh);
}

void vnc_mpi_to_bytes(const gcry_mpi_t value, guchar* result, size_t size)
{
       gcry_error_t error;
       size_t len;
       int i;

       error = gcry_mpi_print(GCRYMPI_FMT_USG, result, size, &len, value);
       if (gcry_err_code (error) != GPG_ERR_NO_ERROR) {
	       VNC_DEBUG ("MPI error: %s", gcry_strerror (error));
	       abort();
       }

       /* right adjust the result
	  68 183 219 160 0 0 0 0 becomes
	  0 0 0 0 68 183 219 160 */
       for(i=size-1;i>(int)size-1-(int)len;--i) {
	       result[i] = result[i-size+len];
       }
       for(;i>=0;--i) {
	       result[i] = 0;
       }
}

gcry_mpi_t vnc_bytes_to_mpi(const guchar* value, size_t size)
{
       gcry_mpi_t ret;
       gcry_error_t error;

       error = gcry_mpi_scan(&ret, GCRYMPI_FMT_USG, value, size, NULL);
       if (gcry_err_code (error) != GPG_ERR_NO_ERROR)
         VNC_DEBUG ("MPI error: %s", gcry_strerror (error));

       return ret;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 *  tab-width: 8
 * End:
 */
