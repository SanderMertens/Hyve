/* $CORTO_GENERATED
 *
 * corto_int.c
 *
 * Code written between the begin and end tags will be preserved when the
 * file is regenerated.
 */

#include "corto.h"

corto_int16 _corto_int_init(corto_int this) {
/* $begin(::corto::lang::int::init) */
    corto_primitive(this)->kind = CORTO_INTEGER;
    return corto_primitive_init((corto_primitive)this);
/* $end */
}
