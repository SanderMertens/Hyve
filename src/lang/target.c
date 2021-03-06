/* This is a managed file. Do not delete this comment. */

#include <corto>

int16_t corto_target_construct(
    corto_target this)
{
    bool modifiers = 0;

    if (this->type->kind == CORTO_COLLECTION) {
        /* Automatically initialize collections in target members */
        modifiers = CORTO_NOT_NULL;
    }

    corto_member actual = corto(CORTO_DECLARE|CORTO_FORCE_TYPE, {
        .parent = this, .id = "actual", .type = corto_member_o});
    if (!actual) {
        goto error;
    }
    if (!corto_check_state(actual, CORTO_VALID)) {
        corto_set_ref(&actual->type, this->type);
        actual->modifiers = modifiers;
        if (!corto(CORTO_DEFINE, {.object = actual})) {
            goto error;
        }
    }

    corto_member target = corto(CORTO_DECLARE|CORTO_FORCE_TYPE, {
        .parent = this, .id = "target", .type = corto_member_o});
    if (!target) {
        goto error;
    }
    if (!corto_check_state(target, CORTO_VALID)) {
        corto_set_ref(&target->type, this->type);
        target->modifiers = modifiers;
        if (!corto(CORTO_DEFINE, {.object = target})) {
            goto error;
        }
    }

    corto_member objective = corto(CORTO_DECLARE|CORTO_FORCE_TYPE, {
        .parent = this, .id = "objective", .type = corto_member_o});
    if (!objective) {
        goto error;
    }
    if (!corto_check_state(objective, CORTO_VALID)) {
        corto_set_ref(&objective->type, this->type);
        objective->modifiers = modifiers;
        if (!corto(CORTO_DEFINE, {.object = objective})) {
            goto error;
        }
    }

    corto_type(this)->reference = TRUE;
    corto_type(this)->flags |= CORTO_TYPE_HAS_TARGET;
    corto_type(this)->attr = CORTO_ATTR_OBSERVABLE | CORTO_ATTR_NAMED | CORTO_ATTR_WRITABLE;

    return safe_corto_struct_construct(this);
error:
    return -1;
}
