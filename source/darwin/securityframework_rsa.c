/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/cal/private/rsa.h>

#include <aws/cal/cal.h>
#include <aws/common/encoding.h>

#include <Security/SecKey.h>
#include <Security/Security.h>

struct sec_rsa_key_pair {
    struct aws_rsa_key_pair base;
    CFAllocatorRef cf_allocator;
    SecKeyRef priv_key_ref;
    SecKeyRef pub_key_ref;
};

static void s_rsa_destroy_key(struct aws_rsa_key_pair *key_pair) {
    if (key_pair == NULL) {
        return;
    }

    struct sec_rsa_key_pair *rsa_key = key_pair->impl;

    if (rsa_key->pub_key_ref) {
        CFRelease(rsa_key->pub_key_ref);
    }

    if (rsa_key->priv_key_ref) {
        CFRelease(rsa_key->priv_key_ref);
    }

    if (rsa_key->cf_allocator) {
        aws_wrapped_cf_allocator_destroy(rsa_key->cf_allocator);
    }

    aws_mem_release(key_pair->allocator, rsa_key);
}

/*
 * return either pointer to a key algo (global consts) or NULL in case algo is
 * not supported.
 */
static int s_map_rsa_encryption_algo_to_sec(enum aws_rsa_encryption_algorithm algorithm, SecKeyAlgorithm *out) {

    switch (algorithm) {
        case AWS_CAL_RSA_ENCRYPTION_PKCS1_5:
            *out = kSecKeyAlgorithmRSAEncryptionPKCS1;
            return AWS_OP_SUCCESS;
        case AWS_CAL_RSA_ENCRYPTION_OAEP_SHA256:
            *out = kSecKeyAlgorithmRSAEncryptionOAEPSHA256;
            return AWS_OP_SUCCESS;
        case AWS_CAL_RSA_ENCRYPTION_OAEP_SHA512:
            *out = kSecKeyAlgorithmRSAEncryptionOAEPSHA512;
            return AWS_OP_SUCCESS;
    }

    return aws_raise_error(AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM);
}

/*
 * return either pointer to a key algo (global consts) or NULL in case algo is
 * not supported.
 */
static int s_map_rsa_signing_algo_to_sec(enum aws_rsa_signing_algorithm algorithm, SecKeyAlgorithm *out) {

    switch (algorithm) {
        case AWS_CAL_RSA_SIGNATURE_PKCS1_5_SHA256:
            *out = kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256;
            return AWS_OP_SUCCESS;
        case AWS_CAL_RSA_SIGNATURE_PSS_SHA256:
            if (__builtin_available(macos 10.13, ios 11.0, tvos 11.0, watchos 4.0, *)) {
                *out = kSecKeyAlgorithmRSASignatureDigestPSSSHA256;
                return AWS_OP_SUCCESS;
            } else {
                return AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM;
            }
    }

    return aws_raise_error(AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM);
}

int s_rsa_encrypt(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_encryption_algorithm algorithm,
    struct aws_byte_cursor plaintext,
    struct aws_byte_buf *out) {
    struct sec_rsa_key_pair *key_pair_impl = key_pair->impl;

    if (key_pair_impl->pub_key_ref == NULL) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "RSA Key Pair is missing Public Key required for encrypt operation.");
        return aws_raise_error(AWS_ERROR_CAL_MISSING_REQUIRED_KEY_COMPONENT);
    }

    SecKeyAlgorithm alg;
    if (s_map_rsa_encryption_algo_to_sec(algorithm, &alg)) {
        return AWS_OP_ERR;
    }

    if (!SecKeyIsAlgorithmSupported(key_pair_impl->pub_key_ref, kSecKeyOperationTypeEncrypt, alg)) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "Algo is not supported for this operation");
        return aws_raise_error(AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM);
    }

    CFDataRef plaintext_ref = CFDataCreateWithBytesNoCopy(key_pair_impl->cf_allocator, 
        plaintext.ptr, plaintext.len, kCFAllocatorNull);
    AWS_FATAL_ASSERT(plaintext_ref);

    CFErrorRef error = NULL;
    CFDataRef ciphertext_ref = SecKeyCreateEncryptedData(key_pair_impl->pub_key_ref, alg, plaintext_ref, &error);

    if (error != NULL) {
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        CFRelease(error);
        goto on_error;
    }

    struct aws_byte_cursor ciphertext_cur =
        aws_byte_cursor_from_array(CFDataGetBytePtr(ciphertext_ref), CFDataGetLength(ciphertext_ref));

    if (aws_byte_buf_append(out, &ciphertext_cur)) {
        aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    if (plaintext_ref != NULL) {
        CFRelease(plaintext_ref);
    }

    if (ciphertext_ref != NULL) {
        CFRelease(ciphertext_ref);
    }

    return AWS_OP_ERR;
}

int s_rsa_decrypt(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_encryption_algorithm algorithm,
    struct aws_byte_cursor ciphertext,
    struct aws_byte_buf *out) {
    struct sec_rsa_key_pair *key_pair_impl = key_pair->impl;

    if (key_pair_impl->priv_key_ref == NULL) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "RSA Key Pair is missing Private Key required for encrypt operation.");
        return aws_raise_error(AWS_ERROR_CAL_MISSING_REQUIRED_KEY_COMPONENT);
    }

    SecKeyAlgorithm alg;
    if (s_map_rsa_encryption_algo_to_sec(algorithm, &alg)) {
        return AWS_OP_ERR;
    }

    if (!SecKeyIsAlgorithmSupported(key_pair_impl->pub_key_ref, kSecKeyOperationTypeDecrypt, alg)) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "Algo is not supported for this operation");
        return aws_raise_error(AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM);
    }

    CFDataRef ciphertext_ref = CFDataCreateWithBytesNoCopy(key_pair_impl->cf_allocator, 
        ciphertext.ptr, ciphertext.len, kCFAllocatorNull);
    AWS_FATAL_ASSERT(ciphertext_ref);

    CFErrorRef error = NULL;
    CFDataRef plaintext_ref = SecKeyCreateDecryptedData(key_pair_impl->priv_key_ref, alg, ciphertext_ref, &error);

    if (error != NULL) {
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        CFRelease(error);
        goto on_error;
    }

    struct aws_byte_cursor plaintext_cur =
        aws_byte_cursor_from_array(CFDataGetBytePtr(plaintext_ref), CFDataGetLength(plaintext_ref));

    if (aws_byte_buf_append(out, &plaintext_cur)) {
        aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    if (plaintext_ref != NULL) {
        CFRelease(plaintext_ref);
    }

    if (ciphertext_ref != NULL) {
        CFRelease(ciphertext_ref);
    }

    return AWS_OP_ERR;
}

int s_rsa_sign(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_signing_algorithm algorithm,
    struct aws_byte_cursor digest,
    struct aws_byte_buf *out) {
    struct sec_rsa_key_pair *key_pair_impl = key_pair->impl;

    if (key_pair_impl->priv_key_ref == NULL) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "RSA Key Pair is missing Private Key required for sign operation.");
        return aws_raise_error(AWS_ERROR_CAL_MISSING_REQUIRED_KEY_COMPONENT);
    }

    SecKeyAlgorithm alg;
    if (s_map_rsa_signing_algo_to_sec(algorithm, &alg)) {
        return AWS_OP_ERR;
    }

    if (!SecKeyIsAlgorithmSupported(key_pair_impl->priv_key_ref, kSecKeyOperationTypeSign, alg)) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "Algo is not supported for this operation");
        return aws_raise_error(AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM);
    }

    CFDataRef digest_ref = CFDataCreateWithBytesNoCopy(key_pair_impl->cf_allocator, digest.ptr, digest.len, kCFAllocatorNull);
    AWS_FATAL_ASSERT(digest_ref);

    CFErrorRef error = NULL;
    CFDataRef signature_ref = SecKeyCreateSignature(key_pair_impl->priv_key_ref, alg, digest_ref, &error);

    if (error != NULL) {
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        CFRelease(error);
        goto on_error;
    }

    struct aws_byte_cursor signature_cur =
        aws_byte_cursor_from_array(CFDataGetBytePtr(signature_ref), CFDataGetLength(signature_ref));

    if (aws_byte_buf_append(out, &signature_cur)) {
        aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        goto on_error;
    }

    CFRelease(digest_ref);
    CFRelease(signature_ref);

    return AWS_OP_SUCCESS;

on_error:
    CFRelease(digest_ref);

    if (signature_ref != NULL) {
        CFRelease(signature_ref);
    }

    return AWS_OP_ERR;
}

int s_rsa_verify(
    const struct aws_rsa_key_pair *key_pair,
    enum aws_rsa_signing_algorithm algorithm,
    struct aws_byte_cursor digest,
    struct aws_byte_cursor signature) {
    struct sec_rsa_key_pair *key_pair_impl = key_pair->impl;

    if (key_pair_impl->pub_key_ref == NULL) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "RSA Key Pair is missing Public Key required for verify operation.");
        return aws_raise_error(AWS_ERROR_CAL_MISSING_REQUIRED_KEY_COMPONENT);
    }

    SecKeyAlgorithm alg;
    if (s_map_rsa_signing_algo_to_sec(algorithm, &alg)) {
        return AWS_OP_ERR;
    }

    if (!SecKeyIsAlgorithmSupported(key_pair_impl->pub_key_ref, kSecKeyOperationTypeVerify, alg)) {
        AWS_LOGF_ERROR(AWS_LS_CAL_RSA, "Algo is not supported for this operation");
        return aws_raise_error(AWS_ERROR_CAL_UNSUPPORTED_ALGORITHM);
    }

    CFDataRef digest_ref = CFDataCreateWithBytesNoCopy(key_pair_impl->cf_allocator, digest.ptr, digest.len, kCFAllocatorNull);
    CFDataRef signature_ref = CFDataCreateWithBytesNoCopy(key_pair_impl->cf_allocator,
        signature.ptr, signature.len, kCFAllocatorNull);
    AWS_FATAL_ASSERT(digest_ref && signature_ref);

    CFErrorRef error = NULL;
    Boolean result = SecKeyVerifySignature(key_pair_impl->pub_key_ref, alg, digest_ref, signature_ref, &error);

    CFRelease(digest_ref);
    CFRelease(signature_ref);

    if (error != NULL) {
        CFRelease(error);
        return aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
    }

    return result == true ? AWS_OP_SUCCESS : aws_raise_error(AWS_ERROR_CAL_SIGNATURE_VALIDATION_FAILED);
}

static struct aws_rsa_key_vtable s_rsa_key_pair_vtable = {
    .destroy = s_rsa_destroy_key,
    .encrypt = s_rsa_encrypt,
    .decrypt = s_rsa_decrypt,
    .sign = s_rsa_sign,
    .verify = s_rsa_verify,
};

struct aws_rsa_key_pair *aws_rsa_key_pair_new_generate_random(
    struct aws_allocator *allocator,
    size_t key_size_in_bits) {

    if (key_size_in_bits < AWS_CAL_RSA_MIN_SUPPORTED_KEY_SIZE ||
        key_size_in_bits > AWS_CAL_RSA_MAX_SUPPORTED_KEY_SIZE || 
        key_size_in_bits % 8 != 0) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct sec_rsa_key_pair *key_pair = aws_mem_calloc(allocator, 1, sizeof(struct sec_rsa_key_pair));

    aws_ref_count_init(&key_pair->base.ref_count, &key_pair->base, aws_rsa_key_pair_destroy);
    key_pair->base.impl = key_pair;
    key_pair->base.allocator = allocator;
    key_pair->cf_allocator = aws_wrapped_cf_allocator_new(allocator);

    CFDataRef sec_key_export_data = NULL;
    CFNumberRef cf_key_size = NULL;
    CFMutableDictionaryRef key_attributes = NULL;

    if (!key_pair->cf_allocator) {
        goto on_error;
    }

    key_attributes = CFDictionaryCreateMutable(key_pair->cf_allocator, 6, NULL, NULL);

    if (!key_attributes) {
        goto on_error;
    }

    CFDictionaryAddValue(key_attributes, kSecAttrKeyType, kSecAttrKeyTypeRSA);
    CFDictionaryAddValue(key_attributes, kSecAttrKeyClass, kSecAttrKeyClassPrivate);
    int32_t key_size_in_bits_i32 = (int32_t)key_size_in_bits;
    cf_key_size = CFNumberCreate(key_pair->cf_allocator, kCFNumberSInt32Type, &key_size_in_bits_i32);

    if (cf_key_size == NULL) {
        goto on_error;
    }

    CFDictionaryAddValue(key_attributes, kSecAttrKeySizeInBits, cf_key_size);

    CFErrorRef error = NULL;

    key_pair->priv_key_ref = SecKeyCreateRandomKey(key_attributes, &error);

    if (error != NULL) {
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        CFRelease(error);
        goto on_error;
    }

    key_pair->pub_key_ref = SecKeyCopyPublicKey(key_pair->priv_key_ref);

    if (key_pair->pub_key_ref == NULL) {
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        goto on_error;
    }

    sec_key_export_data = SecKeyCopyExternalRepresentation(key_pair->priv_key_ref, &error);
    if (error != NULL) {
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        CFRelease(error);
        goto on_error;
    }

    struct aws_byte_cursor key_cur =
        aws_byte_cursor_from_array(CFDataGetBytePtr(sec_key_export_data), CFDataGetLength(sec_key_export_data));

    if (aws_byte_buf_init_copy_from_cursor(&key_pair->base.priv, allocator, key_cur)) {
        goto on_error;
    }

    sec_key_export_data = SecKeyCopyExternalRepresentation(key_pair->pub_key_ref, &error);
    if (error != NULL) {
        aws_raise_error(AWS_ERROR_SYS_CALL_FAILURE);
        CFRelease(error);
        goto on_error;
    }

    key_cur = aws_byte_cursor_from_array(CFDataGetBytePtr(sec_key_export_data), CFDataGetLength(sec_key_export_data));

    if (aws_byte_buf_init_copy_from_cursor(&key_pair->base.pub, allocator, key_cur)) {
        goto on_error;
    }

    key_pair->base.vtable = &s_rsa_key_pair_vtable;
    key_pair->base.key_size_in_bits = key_size_in_bits;

    CFRelease(sec_key_export_data);
    CFRelease(cf_key_size);
    CFRelease(key_attributes);

    return &key_pair->base;

on_error:
    if (key_attributes) {
        CFRelease(key_attributes);
    }

    if (sec_key_export_data) {
        CFRelease(sec_key_export_data);
    }

    if (cf_key_size) {
        CFRelease(cf_key_size);
    }

    s_rsa_destroy_key(&key_pair->base);
    return NULL;
}

struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_private_key_pkcs1_impl(
    struct aws_allocator *allocator,
    struct aws_byte_cursor key) {
    struct sec_rsa_key_pair *key_pair_impl = aws_mem_calloc(allocator, 1, sizeof(struct sec_rsa_key_pair));

    CFMutableDictionaryRef key_attributes = NULL;
    CFDataRef private_key_data = NULL;

    aws_ref_count_init(&key_pair_impl->base.ref_count, &key_pair_impl->base, aws_rsa_key_pair_destroy);
    key_pair_impl->base.impl = key_pair_impl;
    key_pair_impl->base.allocator = allocator;
    key_pair_impl->cf_allocator = aws_wrapped_cf_allocator_new(allocator);
    aws_byte_buf_init_copy_from_cursor(&key_pair_impl->base.priv, allocator, key);

    private_key_data = CFDataCreate(key_pair_impl->cf_allocator, key.ptr, key.len);

    if (private_key_data == NULL) {
        goto on_error;
    }

    key_attributes = CFDictionaryCreateMutable(key_pair_impl->cf_allocator, 6, NULL, NULL);

    if (key_attributes == NULL) {
        goto on_error;
    }

    CFDictionaryAddValue(key_attributes, kSecClass, kSecClassKey);
    CFDictionaryAddValue(key_attributes, kSecAttrKeyType, kSecAttrKeyTypeRSA);
    CFDictionaryAddValue(key_attributes, kSecAttrKeyClass, kSecAttrKeyClassPrivate);

    CFErrorRef error = NULL;
    key_pair_impl->priv_key_ref = SecKeyCreateWithData(private_key_data, key_attributes, &error);

    if (error != NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        CFRelease(error);
        goto on_error;
    }

    key_pair_impl->pub_key_ref = SecKeyCopyPublicKey(key_pair_impl->priv_key_ref);

    CFRelease(key_attributes);
    CFRelease(private_key_data);

    key_pair_impl->base.vtable = &s_rsa_key_pair_vtable;
    key_pair_impl->base.key_size_in_bits = SecKeyGetBlockSize(key_pair_impl->priv_key_ref) * 8;

    return &key_pair_impl->base;

on_error:
    if (private_key_data) {
        CFRelease(private_key_data);
    }

    if (key_attributes) {
        CFRelease(key_attributes);
    }
    aws_byte_buf_clean_up_secure(&key_pair_impl->base.priv);
    aws_byte_buf_clean_up_secure(&key_pair_impl->base.pub);
    s_rsa_destroy_key(&key_pair_impl->base);
    return NULL;
}

struct aws_rsa_key_pair *aws_rsa_key_pair_new_from_public_key_pkcs1_impl(
    struct aws_allocator *allocator,
    struct aws_byte_cursor key) {
    struct sec_rsa_key_pair *key_pair_impl = aws_mem_calloc(allocator, 1, sizeof(struct sec_rsa_key_pair));

    CFMutableDictionaryRef key_attributes = NULL;
    CFDataRef public_key_data = NULL;

    aws_ref_count_init(&key_pair_impl->base.ref_count, &key_pair_impl->base, aws_rsa_key_pair_destroy);
    key_pair_impl->base.impl = key_pair_impl;
    key_pair_impl->base.allocator = allocator;
    key_pair_impl->cf_allocator = aws_wrapped_cf_allocator_new(allocator);
    aws_byte_buf_init_copy_from_cursor(&key_pair_impl->base.pub, allocator, key);

    public_key_data = CFDataCreate(key_pair_impl->cf_allocator, key.ptr, key.len);

    if (public_key_data == NULL) {
        goto on_error;
    }

    key_attributes = CFDictionaryCreateMutable(key_pair_impl->cf_allocator, 10, NULL, NULL);

    if (key_attributes == NULL) {
        goto on_error;
    }

    CFDictionaryAddValue(key_attributes, kSecClass, kSecClassKey);
    CFDictionaryAddValue(key_attributes, kSecAttrKeyType, kSecAttrKeyTypeRSA);
    CFDictionaryAddValue(key_attributes, kSecAttrKeyClass, kSecAttrKeyClassPublic);

    CFErrorRef error = NULL;
    key_pair_impl->pub_key_ref = SecKeyCreateWithData(public_key_data, key_attributes, &error);

    if (error != NULL) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        CFRelease(error);
        goto on_error;
    }

    CFRelease(key_attributes);
    CFRelease(public_key_data);

    key_pair_impl->base.vtable = &s_rsa_key_pair_vtable;
    key_pair_impl->base.key_size_in_bits = SecKeyGetBlockSize(key_pair_impl->pub_key_ref) * 8;

    return &key_pair_impl->base;

on_error:
    if (public_key_data) {
        CFRelease(public_key_data);
    }

    if (key_attributes) {
        CFRelease(key_attributes);
    }
    aws_byte_buf_clean_up_secure(&key_pair_impl->base.priv);
    aws_byte_buf_clean_up_secure(&key_pair_impl->base.pub);
    s_rsa_destroy_key(&key_pair_impl->base);
    return NULL;
}