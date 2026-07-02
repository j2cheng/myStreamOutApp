/* GStreamer
 * Copyright (C) 2010 Wesley Miller <wmiller@sdr.com>
 *
 *
 *  gst_element_print_properties(): a tool to inspect GStreamer
 *                                  element properties
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */


#include <gst/gst.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>

#include "gst_element_print_properties.h"
#include <stdbool.h>
#include "csioCommonShare.h" //for CSIO_LOG


void gst_element_print_properties( GstElement * element )
{
   /////////////////////////////////////////////////////////////////////////////
   //
   // Formatting setup
   //
   //    Change the valuses of c2w, c3w and c4w to adjust the 2nd, 3rd and 4th
   //    column widths, respectively.  The gutter width is fixed at 3 and
   //    alwasys prints as " | ".  Column 1 has a fixed width of 3.
   //
   //    The first two rows for each element's output are its element class
   //    name (e.g. "GstAudioResample") and its element factory name
   //    ("audioresample").  The long element factory name ("Audio resampler")
   //    is in column 4 following the element factory name.
   //
   //    Most properties use this format.  Multivalued items like CAPS, certain
   //    GST_TYPEs and enums are different.
   //
   //      Column 1  contains the rwc, "readable", "writable", "controllable"
   //                flags of the property.
   //      Column 2  contains the property name
   //      Column 3  contains the current value
   //      Column 4  contains the property type, e.g. G_TYPE_INT
   //      Column 5  contains the range, if there is one, and the default.
   //                The range is encosed in parentheses. e.g.  "(1-10)   5"
   //
   //    CAPS, enums, flags and some undefined items have no columns 4 or 5 and
   //    column 3 will contain a description of the item.  Additional rows may
   //    list specific valused (CAPS and flags).
   //
   //    String values are enclosed in double quotes.  A missing right quote
   //    inidicates the string had been truncated.
   //
   //  Screen column
   //  ----+----1----+----2----+----3----+----4----+----5----+----6----+----7----+----8----+----9--->
   //
   //  formatted columns with built in gutters
   //  --- | ---------c2---------- | ---------c3-------- | -----------c4---------- | --> unspecified
   //
   //  <-->|<--- property name --->|<-- current value -->|<-------- type --------->|<----- range and default ----->
   //      | ELEMENT CLASS NAME    | GstAudioResample    |                         |
   //      | ELEMENT FACTORY NAME  | audioresample       | Audio resampler         |
   //  RW- | name                  | "audioResampler"    | G_TYPE_STRING           | null
   //  RW- | qos                   | false               | G_TYPE_BOOLEAN          | false
   //  RW- | quality               | 8                   | G_TYPE_INT              | (0 - 10)   4
   //
   /////////////////////////////////////////////////////////////////////////////

   const guint    c2w = 21;   // column 2 width
   const guint    c3w = 19;   // column 3 width
   const guint    c4w = 23;   // column 4 width

   /////////////////////////////////////////////////////////////////////////////
   // end configuration variables.
   /////////////////////////////////////////////////////////////////////////////

   GParamSpec  **property_specs;
   guint       num_properties, i;
   gboolean    readable;
   gboolean    first_flag;


   g_return_if_fail( element != NULL );

   property_specs = g_object_class_list_properties(
                                      G_OBJECT_GET_CLASS( element ),
                                      &num_properties );

   /*--- draw the header information ---*/
   print_column_titles( c2w, c3w, c4w );
   print_element_info( element, c2w, c3w, c4w );


   for ( i = 0 ; i < num_properties ; i++ )
   {
      GValue      value = { 0, };
      GParamSpec  *param = property_specs[ i ];

      readable   = FALSE;
      first_flag = TRUE;

      g_value_init( &value, param->value_type );

      gchar flags[4];
      flags[0] = '-';
      flags[1] = '-';
      flags[2] = '-';
      flags[3] = 0x0;

      if ( param->flags & G_PARAM_READABLE )
      {
         g_object_get_property( G_OBJECT( element ), param->name, &value );
         readable = TRUE;
         flags[0] = 'r';
      }

      if ( param->flags & G_PARAM_WRITABLE )
         flags[1] = 'w';

      if ( param->flags & GST_PARAM_CONTROLLABLE )
         flags[2] = 'c';

      CSIO_LOG(eLogLevel_error,  "%s |", flags );
      CSIO_LOG(eLogLevel_error,  " %-*s | ", c2w, g_param_spec_get_name( param ));

      switch( G_VALUE_TYPE( &value ))
      {
         case G_TYPE_STRING:  // String
         {
            GParamSpecString *pstring = G_PARAM_SPEC_STRING( param );
            if ( readable )                                        /* current */
            {
               const char  *string_val = g_value_get_string( &value );
               gchar       work_string[100];

               if ( string_val == NULL )
                  sprintf( work_string, "\"%s\"","null" );
               else
                  sprintf( work_string, "\"%s\"", string_val );
               CSIO_LOG(eLogLevel_error,  "%-*.*s", c3w, c3w, work_string );
            }
            else
            {
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );       /* alt current */
            }
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_STRING" );               /* type */

            if ( pstring->default_value == NULL )
               CSIO_LOG(eLogLevel_error,  " | %s", "null" );                         /* default */
            else
               CSIO_LOG(eLogLevel_error,  " | \"%s\"", pstring->default_value );     /* default */
            break;
         }

         case G_TYPE_BOOLEAN: //  Boolean
         {
            GParamSpecBoolean *pboolean = G_PARAM_SPEC_BOOLEAN( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w,
                        ( g_value_get_boolean( &value ) ? "true" : "false" ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_BOOLEAN" );              /* type */
            CSIO_LOG(eLogLevel_error,  " | %s ",                                     /* default */
                     (pboolean->default_value ? "true" : "false"));
            break;
         }

         case G_TYPE_ULONG:  //  Unsigned Long
         {
            GParamSpecULong *pulong = G_PARAM_SPEC_ULONG( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*lu", c3w, g_value_get_ulong( &value ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_ULONG" );                /* type */
            CSIO_LOG(eLogLevel_error,  " | (%lu - %lu)   %lu ",
                     pulong->minimum, pulong->maximum,               /* range */
                     pulong->default_value );                      /* default */
            break;
         }

         case G_TYPE_LONG:  //  Long
         {
            GParamSpecLong *plong = G_PARAM_SPEC_LONG( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*ld", c3w, g_value_get_long( &value ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_LONG" );                 /* type */
            CSIO_LOG(eLogLevel_error,  " | (%ld - %ld)   %ld ",
                     plong->minimum, plong->maximum,                 /* range */
                     plong->default_value );                       /* default */
            break;
         }

         case G_TYPE_UINT:  //  Unsigned Integer
         {
            GParamSpecUInt *puint = G_PARAM_SPEC_UINT( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*u", c3w, g_value_get_uint( &value ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_UINT" );                 /* type */
            CSIO_LOG(eLogLevel_error,  " | (%u - %u)   %u ",
                     puint->minimum, puint->maximum,                 /* range */
                     puint->default_value );                       /* default */
            break;
         }

         case G_TYPE_INT:  //  Integer
         {
            GParamSpecInt *pint = G_PARAM_SPEC_INT( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*d", c3w, g_value_get_int( &value ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_INT" );                  /* type */
            CSIO_LOG(eLogLevel_error,  " | (%d - %d)   %d ",
                     pint->minimum, pint->maximum,                   /* range */
                     pint->default_value );                        /* default */
            break;
         }

         case G_TYPE_UINT64: //  Unsigned Integer64.
         {
            GParamSpecUInt64 *puint64 = G_PARAM_SPEC_UINT64( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*" G_GUINT64_FORMAT,
                        c3w, g_value_get_uint64( &value ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_UINT64" );               /* type */
            CSIO_LOG(eLogLevel_error,  " | (%" G_GUINT64_FORMAT " - %" G_GUINT64_FORMAT ")"
                     "   %" G_GUINT64_FORMAT " ",
                     puint64->minimum, puint64->maximum,             /* range */
                     puint64->default_value );                     /* default */
            break;
         }

         case G_TYPE_INT64: // Integer64
         {
            GParamSpecInt64 *pint64 = G_PARAM_SPEC_INT64( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*" G_GINT64_FORMAT, c3w, g_value_get_int64( &value ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_INT64"  );               /* type */
            CSIO_LOG(eLogLevel_error,  " | (%" G_GINT64_FORMAT " - %" G_GINT64_FORMAT ")"
                     "   %" G_GINT64_FORMAT " ",
                     pint64->minimum, pint64->maximum,               /* range */
                     pint64->default_value );                      /* default */
            break;
         }

         case G_TYPE_FLOAT:  //  Float.
         {
            GParamSpecFloat *pfloat = G_PARAM_SPEC_FLOAT( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*g", c3w, g_value_get_float( &value ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_FLOAT" );                /* type */
            CSIO_LOG(eLogLevel_error,  " | (%g - %g)   %g ",
                      pfloat->minimum, pfloat->maximum,              /* range */
                      pfloat->default_value );                     /* default */
            break;
         }

         case G_TYPE_DOUBLE:  //  Double
         {
            GParamSpecDouble *pdouble = G_PARAM_SPEC_DOUBLE( param );
            if ( readable )                                        /* current */
               CSIO_LOG(eLogLevel_error,  "%-*g", c3w, g_value_get_double( &value ));
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );
            CSIO_LOG(eLogLevel_error,  " | %-*s", c4w, "G_TYPE_DOUBLE" );               /* type */
            CSIO_LOG(eLogLevel_error,  " | (%g - %g)   %g ",
                     pdouble->minimum, pdouble->maximum,             /* range */
                     pdouble->default_value );                     /* default */
            break;
         }

         default:
         if ( param->value_type == GST_TYPE_CAPS )
         {
            const GstCaps *caps = gst_value_get_caps( &value );
            if ( !caps )
               CSIO_LOG(eLogLevel_error,  "%-*s | %-*.*s |",
                        c3w, "Caps (NULL)",
                        c4w, c4w, " " );
            else
            {
               gchar   prefix_string[ 100 ];
               sprintf( prefix_string, "    | %-*.*s | ", c2w, c2w, " " );
               print_caps( caps, prefix_string );
            }
         }

         else if ( G_IS_PARAM_SPEC_ENUM( param ))
         {
            GParamSpecEnum *penum = G_PARAM_SPEC_ENUM( param );
            GEnumValue     *values;
            guint          j = 0;
            gint           enum_value;
            const gchar    *def_val_nick = "", *cur_val_nick = "";
            gchar          work_string[100];

            values = G_ENUM_CLASS( g_type_class_ref(param->value_type))->values;
            enum_value = g_value_get_enum( &value );

            while ( values[ j ].value_name )
               {
               if ( values[ j ].value == enum_value )
                  cur_val_nick = values[ j ].value_nick;
               if ( values[ j ].value == penum->default_value )
                  def_val_nick = values[ j ].value_nick;
               j++;
               }

            sprintf( work_string, "%d, \"%s\"", enum_value, cur_val_nick );
            CSIO_LOG(eLogLevel_error,  "%-*.*s", c3w, c3w, work_string );
            CSIO_LOG(eLogLevel_error,  " | Enum \"%s\" : %d, \"%s\"",
                     g_type_name( G_VALUE_TYPE( &value )),
                     penum->default_value, def_val_nick );
         }

         else if ( G_IS_PARAM_SPEC_FLAGS( param ))
         {
            GParamSpecFlags   *pflags = G_PARAM_SPEC_FLAGS( param );
            GFlagsValue       *vals;
            gchar             *cur, *def;
            gchar             work_string[512];

            vals = pflags->flags_class->values;
            cur = flags_to_string( vals,
                                   g_value_get_flags( &value ));   /* current */
            def = flags_to_string( vals, pflags->default_value );  /* default */

            /* current */
            sprintf( work_string, "0x%08x, \"%s\"",
                     g_value_get_flags( &value ), cur  );
            CSIO_LOG(eLogLevel_error,  "%-*.*s", c3w, c3w, work_string );

            /* type */
            sprintf( work_string, "Flags \"%s\"",
                     g_type_name( G_VALUE_TYPE( &value )) );
            CSIO_LOG(eLogLevel_error,  "%-*.*s", c4w, c4w, work_string );

            /* default */
            CSIO_LOG(eLogLevel_error,  " | 0x%08x, \"%s\"",
                     pflags->default_value, def );

            /* values list */
            while ( vals[0].value_name )
            {
               sprintf( work_string, "\n    | %-*.*s |   (0x%08x): %-16s - %s",
                        c2w, c2w, "",
                        vals[0].value, vals[0].value_nick, vals[0].value_name );
               CSIO_LOG(eLogLevel_error,  "%s", work_string );
               ++vals;
            }

            g_free( cur );
            g_free( def );
         }

         else if ( G_IS_PARAM_SPEC_OBJECT( param ))
         {
            CSIO_LOG(eLogLevel_error,  "%-*.*s | Object of type \"%s\"",
                     c3w, c3w,
                     g_type_name( param->value_type ),
                     g_type_name( param->value_type ) );
         }

         else if ( G_IS_PARAM_SPEC_BOXED( param ))
         {
            CSIO_LOG(eLogLevel_error,  "%-*.*s | Boxed pointer of type \"%s\"",
                     c3w, c3w,
                     g_type_name( param->value_type ),
                     g_type_name( param->value_type ) );
         }

         else if ( G_IS_PARAM_SPEC_POINTER( param ))
         {
            if ( param->value_type != G_TYPE_POINTER )
            {
               CSIO_LOG(eLogLevel_error,  "%-*.*s | Pointer of type \"%s\"",
                        c3w, c3w,
                        g_type_name( param->value_type ),
                        g_type_name( param->value_type ) );
            }
            else
            {
               CSIO_LOG(eLogLevel_error,  "%-*.*s |", c3w, c3w, "Pointer." );
            }
         }

         // GValueArray is deprecated in GLib but still emitted by some
         // GStreamer element properties, so we must keep handling it.
         // Suppress only the deprecation diagnostics for this branch.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-W#pragma-messages"
         else if ( param->value_type == G_TYPE_VALUE_ARRAY )
         {
            GParamSpecValueArray *pvarray = G_PARAM_SPEC_VALUE_ARRAY( param );
            if ( pvarray->element_spec )
            {
               CSIO_LOG(eLogLevel_error,  "%-*.*s :Array of GValues of type \"%s\"",
                        c3w, c3w,
                        g_type_name( pvarray->element_spec->value_type ),
                        g_type_name( pvarray->element_spec->value_type ) );
            }
            else
            {
               CSIO_LOG(eLogLevel_error,  "%-*.*s :", c3w, c3w, "Array of GValues" );
            }
         }
#pragma GCC diagnostic pop

         else if ( GST_IS_PARAM_SPEC_FRACTION( param ))
         {
            GstParamSpecFraction   *pfraction = GST_PARAM_SPEC_FRACTION( param );
            gchar                  work_string[100];

            if ( readable )                                        /* current */
            {
               sprintf( work_string, "%d/%d",
                        gst_value_get_fraction_numerator( &value ),
                        gst_value_get_fraction_denominator( &value ));
               CSIO_LOG(eLogLevel_error,  "%-*.*s", c3w, c3w, work_string );
            }
            else
               CSIO_LOG(eLogLevel_error,  "%-*s", c3w, "<not readable>" );

            CSIO_LOG(eLogLevel_error,  " | %-*.*s",                                     /* type */
                     c3w, c3w,
                     " Fraction. " );
            CSIO_LOG(eLogLevel_error,  " | (%d/%d - %d/%d)",                           /* range */
                     pfraction->min_num, pfraction->min_den,
                     pfraction->max_num, pfraction->max_den );
            CSIO_LOG(eLogLevel_error,  "   %d/%d ",                                  /* default */
                     pfraction->def_num, pfraction->def_den );
         }

         else if ( G_IS_PARAM_SPEC_BOXED( param ))
         {
            CSIO_LOG(eLogLevel_error,  "%-*.*s | Boxed of type \"%s\"",
                     c3w, c3w,
                     g_type_name( param->value_type ),
                     g_type_name( param->value_type ) );
         }

         else
         {
            CSIO_LOG(eLogLevel_error,  "Unknown type %ld \"%s\"",
                     (glong)param->value_type,
                     g_type_name( param->value_type ));

         }
         break;
      }

      if ( !readable )
         CSIO_LOG(eLogLevel_error,  " Write only\n" );
      else
         CSIO_LOG(eLogLevel_error,  "\n" );

      g_value_reset( &value );
   }

   if ( 0 == num_properties )
      CSIO_LOG(eLogLevel_error,  "  none\n" );

   g_free( property_specs );
}

//------------------------------------------------------------------------------
void   print_column_titles( guint c2w, guint c3w, guint c4w )
   {
      //////////////////////////////////////////////////////////////////////////
      //
      // Create Header for property listing
      // RWF | --- element name ---- | ---------c3-------- | -----------c4---------- | --> unspecified
      //
      //////////////////////////////////////////////////////////////////////////
      gchar   work_string[200];
      gchar   dashes[] = "-----------------------------";
      gint    llen = 0;
      gint    rlen = 0;

      /*--- column 1 - RWC ---*/
      sprintf( work_string, "<-->|<" );

      /*--- column 2 - property name ---*/
      llen = (c2w - 15) / 2;               /* width of " property name " = 15 */
      rlen = c2w - 15 - llen;

      strncat( work_string, dashes, llen );
      strcat( work_string, " property name " );
      strncat( work_string, dashes, rlen );
      strcat( work_string, ">|<" );

      /*--- column 3 - current value ---*/
      llen = (c3w - 15) / 2;               /* width of " current value " = 15 */
      rlen = c3w - 15 - llen;

      strncat( work_string, dashes, llen );
      strcat( work_string, " current value " );
      strncat( work_string, dashes, rlen );
      strcat( work_string, ">|<" );

      /*--- column 4 - type ---*/
      llen = (c4w - 6) / 2;                          /* width of " type " = 6 */
      rlen = c4w - 6 - llen;

      strncat( work_string, dashes, llen );
      strcat( work_string, " type " );
      strncat( work_string, dashes, rlen );
      strcat( work_string, ">|<" );

      /*--- column 5 - range and default ---*/
      strcat( work_string, "----- range and default ----->" );

      CSIO_LOG(eLogLevel_error, "\n%s\n", work_string );
   }

//------------------------------------------------------------------------------
void  print_element_info( GstElement *element, guint c2w, guint c3w, guint c4w  )
{
   /////////////////////////////////////////////////////////////////////////////
   //
   // Print element factory and class information as part of each header
   //
   /////////////////////////////////////////////////////////////////////////////
   gchar               work_string[100];
   GstElementFactory   *factory = gst_element_get_factory( element );

   sprintf( work_string, "ELEMENT CLASS NAME" );
   CSIO_LOG(eLogLevel_error,  "    | %-*s",   c2w, work_string );
   CSIO_LOG(eLogLevel_error,  " | %-*s",      c3w, g_type_name( G_OBJECT_TYPE( element )) );
   CSIO_LOG(eLogLevel_error,  " | %-*s | \n", c4w, "" );


   sprintf( work_string, "ELEMENT FACTORY NAME" );
   CSIO_LOG(eLogLevel_error,  "    | %-*s",   c2w, work_string );

   CSIO_LOG(eLogLevel_error,  " | %-*s",      c3w, gst_plugin_feature_get_name( GST_PLUGIN_FEATURE( factory ) ));
   CSIO_LOG(eLogLevel_error,  " | %-*s | \n", c4w,  gst_element_factory_get_longname( factory ));

// "Audio Resampler"   CSIO_LOG(eLogLevel_error,  " | %-*s",      c3w, gst_element_factory_get_longname( gst_element_get_factory( element )) );


}

//------------------------------------------------------------------------------
gchar*   flags_to_string( GFlagsValue *vals, guint flags )
{
   /////////////////////////////////////////////////////////////////////////////
   //
   // List individual flags in separate rows
   //
   /////////////////////////////////////////////////////////////////////////////
    GString *s = NULL;
    guint flags_left, i;

    /* first look for an exact match and count the number of values */
    for ( i = 0; vals[i].value_name != NULL; ++i )
    {
       if ( vals[i].value == flags )
          return g_strdup( vals[i].value_nick );
    }

   s = g_string_new( NULL );

   /* we assume the values are sorted from lowest to highest value */
   flags_left = flags;
   while ( i > 0 )
   {
       --i;
       if (0 != vals[i].value && (flags_left & vals[i].value) == vals[i].value)
       {
          if ( 0 < s->len )
             g_string_append( s, " | " );
          g_string_append( s, vals[ i ].value_nick );
          flags_left -= vals[i].value;
          if ( 0 == flags_left )
             break;
       }
   }

   if ( 0 == s->len )
      g_string_assign( s, "(none)" );

   return g_string_free( s, FALSE );
}

//Note: this function added by AI-----------------------------------------------
void print_caps( const GstCaps * caps, const gchar * pfx )
{
   /////////////////////////////////////////////////////////////////////////////
   //
   // Print GStreamer capabilities
   //
   /////////////////////////////////////////////////////////////////////////////
   if (!caps) {
      CSIO_LOG(eLogLevel_error, "%s(NULL)", pfx ? pfx : "");
      return;
   }

   guint i, num_structures = gst_caps_get_size(caps);
   for (i = 0; i < num_structures; ++i) {
      GstStructure *structure = gst_caps_get_structure(caps, i);
      gchar *structure_str = gst_structure_to_string(structure);
      CSIO_LOG(eLogLevel_error, "%s%s", pfx ? pfx : "", structure_str ? structure_str : "(null)");
      g_free(structure_str);
   }
}

