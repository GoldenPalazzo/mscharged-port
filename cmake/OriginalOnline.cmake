include_guard(GLOBAL)

if(WIN32)
    return()
endif()
# A static archive: the host link pulls only the members that define what
# the game module imports, and --gc-sections then drops the functions
# nothing reaches, as CodeWarrior's linker did for the retail binary.
add_library(charged_original_online_sources STATIC EXCLUDE_FROM_ALL
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_account.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_auth_interface.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_base64.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_common.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_encsession.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_error.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_friend.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_ghttp.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_init.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwci_np_math.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_login.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_main.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_match.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_memfunc.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_nastime.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_nonport.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_ranking.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_ranksession.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_report.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/dwc/dwc_transport.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsAvailable.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsCrypt.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsLargeInt.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsRC4.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsSHA1.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsSSL.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/common/gsXML.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/darray.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpBuffer.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpCallbacks.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpCommon.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpConnection.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpEncryption.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpMain.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpPost.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/ghttp/ghttpProcess.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gp.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiBuddy.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiBuffer.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpi.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiCallback.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiConnect.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiInfo.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiOperation.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiPeer.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiProfile.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiSearch.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiTransfer.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiUnique.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP/gpiUtility.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gstats/gbucket.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gstats/gstats.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Auth.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Buffer.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Callback.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Connection.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Main.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Message.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Socket.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/gt2/gt2Utility.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/hashtable.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/md5c.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/natneg/NATify.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/natneg/natneg.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/nonport.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/qr2/qr2.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/qr2/qr2regkeys.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_crypt.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_queryengine.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_serverbrowsing.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_server.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/serverbrowsing/sb_serverlist.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/d_nhttp.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/d_nhttp_common.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/d_nhttp_private.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_bgnend.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_control.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_list.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_os_RVL.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_recvbuf.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_request.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_response.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_socket_RVL.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_stdlib_RVL.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/nhttp/NHTTP_thread.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/net/hmac.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/net/md5.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/net/neterrorcode.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/net/NETVersion.c"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/net/wireless_macaddr.c")
add_dependencies(charged_original_online_sources verify_prepared)
target_include_directories(charged_original_online_sources PRIVATE
    src
    "${MSCHARGED_PREPARED}/include"
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include"
    "${MSCHARGED_PREPARED}/src/RVL_SDK"
    "${MSCHARGED_PREPARED}/src/RVL_SDK/gamespy/GP"
    "${MSCHARGED_AURORA_PREPARED}/include")
target_compile_definitions(charged_original_online_sources PRIVATE
    MSCHARGED_NATIVE=1
    TARGET_PC=1
    _REVOLUTION=1)
target_link_libraries(charged_original_online_sources PRIVATE aurora::os)
target_compile_features(charged_original_online_sources PRIVATE c_std_17)
target_compile_options(charged_original_online_sources PRIVATE
    -ffunction-sections
    -fdata-sections
    -fsigned-char
    -fno-strict-aliasing
    -Wno-unknown-pragmas
    -Wno-unused-value
    -Werror=pointer-to-int-cast
    -Werror=int-to-pointer-cast
    -Werror=implicit-function-declaration)

# The host exports every global symbol to the game module, and an exported
# symbol is never collected. The module calls only DWC and NET, so GameSpy and
# NHTTP stay hidden: --gc-sections can then drop their entry points that
# nothing calls, which the retail linker had already removed together with
# the internals they reference.
get_target_property(_online_sources charged_original_online_sources SOURCES)
list(FILTER _online_sources INCLUDE REGEX "/RVL_SDK/(gamespy|nhttp)/")
set_source_files_properties(${_online_sources} TARGET_DIRECTORY charged_original_online_sources
    PROPERTIES COMPILE_OPTIONS -fvisibility=hidden)

option(MSCHARGED_DIAGNOSTIC_ONLINE_SDK
    "Link the original DWC, GameSpy, NHTTP and NET sources into the host" OFF)
if(MSCHARGED_DIAGNOSTIC_ONLINE_SDK)
    if(NOT TARGET charged_original_main_credits_host OR NOT TARGET charged_network)
        message(FATAL_ERROR "The online SDK links into the original host beside the SO backend")
    endif()
    include(cmake/OriginalModuleLinkage.cmake)
    target_link_libraries(charged_original_main_credits_host PRIVATE
        charged_original_online_sources charged_network)
    # What the game module imports from DWC and NET.
    foreach(_symbol IN ITEMS
            DWC_AddMatchKeyInt DWC_Base64Decode DWC_CancelMatching DWC_CheckDirtyFlag
            DWC_CheckUserData DWC_ClearDirtyFlag DWC_ClearError DWC_CloseAllConnectionsHard
            DWC_ConnectToAnybodyAsync DWC_CreateUserData DWC_GetDateTime DWC_GetFriendDataType
            DWC_GetIngamesnCheckResult DWC_GetLastErrorEx DWC_GetMatchIntValue
            DWC_GetMOMinCompState DWC_GetMyAID DWC_GetNumConnectionHost DWC_Init
            DWC_InitFriendsMatch DWC_IsBuddyFriendData DWC_IsValidCancelMatching
            DWC_IsValidFriendData DWC_LoginAsync DWC_ProcessFriendsMatch DWC_ReportUserData
            DWC_RnkGetScoreAsync DWC_RnkProcess DWC_RnkPutScoreAsync DWC_RnkResGetRow
            DWC_RnkResGetRowCount DWC_RnkShutdown DWC_SendUnreliable DWC_SetBuddyFriendCallback
            DWC_SetConnectionClosedCallback DWC_SetFriendStatusCallback DWC_SetMatchingOption
            DWC_SetOwnStatusData DWC_SetRecvBuffer DWC_SetReportLevel DWC_SetUserRecvCallback
            DWC_Shutdown DWC_ShutdownFriendsMatch DWC_UpdateServersAsync
            NETGetMD5Interface NETHMACGetDigest NETHMACInit NETHMACUpdate)
        mscharged_require_original_host_symbol(charged_original_main_credits_host INTERFACE "${_symbol}")
    endforeach()
endif()
