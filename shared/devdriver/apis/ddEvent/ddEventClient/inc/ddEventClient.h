/* Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. */

#ifndef DD_EVENT_CLIENT_HEADER
#define DD_EVENT_CLIENT_HEADER

#include <ddEventClientApi.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Attempts to create a new client object with the provided creation information
DD_RESULT ddEventClientCreate(
    const DDEventClientCreateInfo* pInfo,     /// [in]  Create info
    DDEventClient*                 phClient); /// [out] Handle to the new client object

/// Destroys an existing client object
void ddEventClientDestroy(
    DDEventClient hClient); /// [in] Handle to the existing client object

/// Attempts to read incoming event from the server
///
/// Any data received through the read operation will be returned via the event data callback that was provided during
/// client creation.
DD_RESULT ddEventClientReadEventData(
    DDEventClient hClient,      /// [in] Handle to an existing client object
    uint32_t      timeoutInMs); /// Timeout in milliseconds

/// Attempts to query and return all known providers from the remote server
DD_RESULT ddEventClientQueryProviders(
    DDEventClient                 hClient,   /// [in] Handle to an existing client object
    const DDEventProviderVisitor* pVisitor); /// [in] Provider visitor to return provider data through

/// Attempts to configure the state of the providers on the remote server
///
/// Providers on the remote server will be updated to reflect the new configuration
DD_RESULT ddEventClientConfigureProviders(
    DDEventClient                 hClient,      /// [in] Handle to an existing client object
    size_t                        numProviders, /// Number of items in the pProviders array
    const DDEventProviderDesc*    pProviders);  /// [in] Array of provider descriptions to send to the server

/// Attempts to fully enable all specified providers on the remote server
///
/// This will enable the providers themselves and all individual events supported by them
DD_RESULT ddEventClientEnableProviders(
    DDEventClient   hClient,        /// [in] Handle to an existing client object
    size_t          numProviderIds, /// Number of items in the pProviderIds array
    const uint32_t* pProviderIds);  /// [in] Array of provider ids to send to the server

/// Attempts to fully disable all specified providers on the remote server
///
/// This will disable the providers themselves and all individual events supported by them
DD_RESULT ddEventClientDisableProviders(
    DDEventClient   hClient,        /// [in] Handle to an existing client object
    size_t          numProviderIds, /// Number of items in the pProviderIds array
    const uint32_t* pProviderIds);  /// [in] Array of provider ids to send to the server

#ifdef __cplusplus
} // extern "C"
#endif

#endif // ! DD_EVENT_CLIENT_HEADER