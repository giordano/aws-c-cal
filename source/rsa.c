/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/private/rsa.h>

#include <aws/cal/cal.h>
#include <aws/cal/private/der.h>

typedef struct aws_rsa_key_pair *(
    aws_rsa_key_pair_new_from_public_key_fn)(struct aws_allocator *allocator, struct aws_byte_cursor public_key);

typedef struct aws_rsa_key_pair *(
    aws_rsa_key_pair_new_from_private_key_fn)(struct aws_allocator *allocator, struct aws_byte_cursor private_key);

#ifndef BYO_CRYPTO

extern struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_public_key_pkcs1_impl(
    struct aws_allocator *allocator,
    struct aws_byte_cursor public_key);

extern struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_private_key_pkcs1_impl(
    struct aws_allocator *allocator,
    struct aws_byte_cursor private_key);

#else  /* BYO_CRYPTO */

struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_public_key_pkcs1_impl(
    struct aws_allocator *allocator,
    struct aws_byte_cursor public_key) {
    (void)allocator;
    (void)public_key abort();
}

struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_private_pkcs1_impl(
    struct aws_allocator *allocator,
    struct aws_byte_cursor private_key) {
    (void)allocator;
    (void)private_key;
    abort();
}
#endif /* BYO_CRYPTO */

static aws_rsa_key_pair_new_from_public_key_fn *s_rsa_key_pair_new_from_public_key_pkcs1_fn =
    aws_rsa_key_pair_new_from_public_key_pkcs1_impl;

static aws_rsa_key_pair_new_from_private_key_fn *s_rsa_key_pair_new_from_private_key_pkcs1_fn =
    aws_rsa_key_pair_new_from_private_key_pkcs1_impl;

struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_public_key_pkcs1(
    struct aws_allocator *allocator,
    struct aws_byte_cursor public_key) {
    return s_rsa_key_pair_new_from_public_key_pkcs1_fn(allocator, public_key);
}

struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_private_key_pkcs1(
    struct aws_allocator *allocator,
    struct aws_byte_cursor private_key) {
    return s_rsa_key_pair_new_from_private_key_pkcs1_fn(allocator, private_key);
}

void aws_rsa_key_pair_destroy(void *key_pair) {
    struct aws_rsa_key_pair *base = key_pair;

    aws_byte_buf_clean_up_secure(&base->priv);
    aws_byte_buf_clean_up_secure(&base->pub);

    AWS_FATAL_ASSERT(base->vtable->destroy && "RSA KEY PAIR destroy function must be included on the vtable");
    base->vtable->destroy(key_pair);
}

struct aws_rsa_key_pair *aws_rsa_key_pair_acquire(struct aws_rsa_key_pair *key_pair) {
    return aws_ref_count_acquire(&key_pair->ref_count);
}

struct aws_rsa_key_pair *aws_rsa_key_pair_release(struct aws_rsa_key_pair *key_pair) {
    if (key_pair != NULL) {
        aws_ref_count_release(&key_pair->ref_count);
    }
    return NULL;
}

size_t aws_rsa_key_pair_max_encrypt_plaintext_size(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_encryption_algorithm algorithm) {
    /*
     * Per rfc8017, max size of plaintext for encrypt operation is as follows:
     * PKCS1-v1_5: (key size in bytes) - 11
     * OAEP: (key size in bytes) - 2 * (hash bytes) - 2
     */

    size_t key_size_in_bytes = key_pair->key_size_in_bits / 8;
    switch (algorithm) {
        case AWS_CAL_RSA_ENCRYPTION_PKCS1_5:
            return key_size_in_bytes - 11;
        case AWS_CAL_RSA_ENCRYPTION_OAEP_SHA256:
            return key_size_in_bytes - 2 * (256 / 8) - 2;
        case AWS_CAL_RSA_ENCRYPTION_OAEP_SHA512:
            return key_size_in_bytes - 2 * (512 / 8) - 2;
        default:
            AWS_FATAL_ASSERT("Unsupported RSA Encryption Algorithm");
    }

    return 0;
}

int aws_rsa_key_pair_encrypt(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_encryption_algorithm algorithm,
    struct aws_byte_cursor plaintext,
    struct aws_byte_buf *out) {
    AWS_PRECONDITION(key_pair);
    AWS_PRECONDITION(out);

    if (AWS_UNLIKELY(aws_rsa_key_pair_max_encrypt_plaintext_size(key_pair, algorithm) < plaintext.len)) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "Unexpected buffer size. For RSA, ciphertext  is expected to match block size");
        return aws_raise_error(AWS_ERROR_CAL_BUFFER_TOO_LARGE_FOR_ALGORITHM);
    }

    return key_pair->vtable->encrypt(key_pair, algorithm, plaintext, out);
}

AWS_CAL_API int aws_rsa_key_pair_decrypt(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_encryption_algorithm algorithm,
    struct aws_byte_cursor ciphertext,
    struct aws_byte_buf *out) {
    AWS_PRECONDITION(key_pair);
    AWS_PRECONDITION(out);

    if (AWS_UNLIKELY(ciphertext.len != (key_pair->key_size_in_bits / 8))) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "Unexpected buffer size. For RSA, ciphertext is expected to match block size.");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    return key_pair->vtable->decrypt(key_pair, algorithm, ciphertext, out);
}

int aws_rsa_key_pair_sign_message(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_signing_algorithm algorithm,
    struct aws_byte_cursor digest,
    struct aws_byte_buf *out) {
    AWS_PRECONDITION(key_pair);
    AWS_PRECONDITION(out);

    return key_pair->vtable->sign(key_pair, algorithm, digest, out);
}

int aws_rsa_key_pair_verify_signature(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_signing_algorithm algorithm,
    struct aws_byte_cursor digest,
    struct aws_byte_cursor signature) {
    AWS_PRECONDITION(key_pair);

    return key_pair->vtable->verify(key_pair, algorithm, digest, signature);
}

size_t aws_rsa_key_pair_block_length(struct aws_rsa_key_pair *key_pair) {
    AWS_PRECONDITION(key_pair);
    return key_pair->key_size_in_bits / 8;
}

size_t aws_rsa_key_pair_signature_length(const struct aws_rsa_key_pair *key_pair) {
    AWS_PRECONDITION(key_pair);
    return key_pair->key_size_in_bits / 8;
}

int aws_rsa_key_pair_get_public_key(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_key_export_format format,
    struct aws_byte_cursor *out) {
    (void)format; /* ignore format for now, since only pkcs1 is supported. */
    AWS_PRECONDITION(key_pair);
    AWS_PRECONDITION(out);

    *out = aws_byte_cursor_from_buf(&key_pair->pub);
    return AWS_OP_SUCCESS;
}

int aws_rsa_key_pair_get_private_key(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_key_export_format format,
    struct aws_byte_cursor *out) {
    (void)format; /* ignore format for now, since only pkcs1 is supported. */
    AWS_PRECONDITION(key_pair);
    AWS_PRECONDITION(out);

    *out = aws_byte_cursor_from_buf(&key_pair->priv);
    return AWS_OP_SUCCESS;
}

int aws_der_decoder_load_private_rsa_pkcs1(struct aws_der_decoder *decoder, struct s_rsa_private_key_pkcs1 *out) {

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_type(decoder) != AWS_DER_SEQUENCE) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    struct aws_byte_cursor version_cur;
    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &version_cur)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (version_cur.len != 1 || version_cur.ptr[0] != 0) {
        return aws_raise_error(AWS_ERROR_CAL_UNSUPPORTED_KEY_FORMAT);
    }
    out->version = 0;

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &(out->modulus))) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &out->publicExponent)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &out->privateExponent)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &out->prime1)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &out->prime2)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &out->exponent1)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &out->exponent2)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &out->coefficient)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    return AWS_OP_SUCCESS;
}

int aws_der_decoder_load_public_rsa_pkcs1(struct aws_der_decoder *decoder, struct s_rsa_public_key_pkcs1 *out) {
    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_type(decoder) != AWS_DER_SEQUENCE) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &(out->modulus))) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    if (!aws_der_decoder_next(decoder) || aws_der_decoder_tlv_integer(decoder, &out->publicExponent)) {
        return aws_raise_error(AWS_ERROR_CAL_MALFORMED_ASN1_ENCOUNTERED);
    }

    return AWS_OP_SUCCESS;
}