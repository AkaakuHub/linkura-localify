package io.github.chocolzs.linkura.localify.ipc

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class WebRtcSignalingMessage(
    @SerialName("type") val type: String,
    @SerialName("sdp") val sdp: String? = null,
    @SerialName("sdpType") val sdpType: String? = null,
    @SerialName("candidate") val candidate: String? = null,
    @SerialName("sdpMid") val sdpMid: String? = null,
    @SerialName("sdpMLineIndex") val sdpMLineIndex: Int? = null
)

