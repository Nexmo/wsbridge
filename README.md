# wsbridge

**WSBridge** is a [FreeSWITCH](https://freeswitch.com/) module that bridges SIP/RTP endpoints to [WebSocket](https://tools.ietf.org/html/rfc6455) Audio Endpoints.

**WSBridge** is designed to handle incoming SIP calls and bridge their media with outgoing WebSocket connections. It currently does not support handling calls generated from a WebSocket endpoint towards SIP.

**WSBridge** behaves as a FreeSWITCH endpoint, and receives information about the media options and the WebSocket endpoint via custom SIP headers.

## Prerequisites

### libwebsockets

[libwebsockets](https://libwebsockets.org/) 2.4

To install libwebsockets:
```
	git clone git@github.com:warmcat/libwebsockets.git
	cd libwebsockets
	git checkout -v v2.4-stable remotes/origin/v2.4-stable
	mkdir build ; cd build ; cmake .. -DCMAKE_BUILD_TYPE=DEBUG ; make ; make install ; ldconfig ;
```

## SIP Headers

**WSBrigde** reads information about the required properties (audio codec, payload type) and target WebSocket endpoint via custom SIP headers in the incoming SIP INVITE request.

To place an outbound WebSocket connection, the following SIP headers need to be set (and they **must** be URL encoded):

- **"P-wsbridge-websocket-uri"** (MANDATORY): A WebSocket (`ws://`) or secure WebSocket (`wss://`) URI to address the target WebSocket endpoint. Example: `P-wsbridge-websocket-uri: ws://soundboard.nexmodev.com/`

- **"P-wsbridge-websocket-headers"** (optional): A key/value JSON object that will be appended to the default headers upon connection of the WebSocket. This allows the calling party to pass custom data, like To/From numbers. Example: `P-wsbridge-websocket-headers: "hello there. how's it going?"`

- **"P-wsbridge-websocket-content-type"** (optional): One of the supported audio formats, defined below. Example: `P-wsbridge-websocket-content-type: audio%2Fl16%3Brate%3D16000`

	If they are not sent in the SIP INVITE, they may be set in the Freeswitch dialplan, e.g.:

```
	<action application="set" data="P-wsbridge-websocket-uri=ws://soundboard.nexmodev.com/" />

	<action application="set" data="P-wsbridge-websocket-content-type=audio%2Fl16%3Brate%3D16000"/>

	<action application="set" data="P-wsbridge-websocket-headers={\"text\":\"hello there. how's it going?\"}"/>
```

## Calling the WSBridge endpoint from the dialplan . 

```
<extension name="freeswitch_wsbridge">
	<condition field="destination_number" expression="^wsbridge$">
		 <!-- optionally force values that otherwise would be read from the SIP headers here. this will make WSBridge only conenct to this WS URI:  -->
		 <!-- <action application="set" data="P-wsbridge-websocket-uri=ws://soundboard.nexmodev.com/" /> --> 
		 <action application="bridge" data="wsbridge"/>
	 </condition>
</extension>

```

## WebSocket Protocol


Upon WebSocket connection establishment, **WSBridge** sends an Initial content JSON message over the socket, with the selected content-type and the optional headers, e.g.:

```
	{
		‘content-type’:’audio/l16;rate=16000’,
		‘name_1’: ‘value_1’,
		‘name_2’: ‘value_2’
	}
```

This is then followed by a sequence of audio frames of the selected content type and rate, one RTP frame per WebSocket frame.

Additionally, the same WebSocket connection can be used to receive audio - any data received will be packetized and forwarded as RTP. **WSBridge** expects data from the WebSocket correctly packetized, according to the `content-type` set on the connection.

Data is only sent and received when it is available. If there is a period of silence **WSBridge** will not send any data.

If **WSBridge** doesn't receive audio from the WebSocket, then it will generate comfort noise locally, to fill the gap, and send it to the RTP stream.

To end the session, one can either close the WebSocket connection (from the server), or end the SIP leg.

## Audio Formats

The acceptable `content-type` values are:
- audio/l16;rate=16000 - signed linear 16khz
- audio/l16;rate=8000 - signed linear 8khz

# Configuration

The **WSBridge** module must be added to the list in `autoload_configs/modules.conf.xml`, as:

`<load module="mod_wsbridge"/>`

If you want to quickly load or reload the module from the CLI:


```reload mod_wsbridge```

or

```fs_cli -x 'reload mod_wsbridge'```


# Rx Flow Control

In previous versions, the WebSocket endpoint could not send all the payload at once if that was larger than the receiving buffer. The buffer would circle around and lose the first part of the data.

**WSBridge** was changed to make use of the WebSocket flow control, as supported by *libwebsockets*. This allows throttling the consumed data.

So now, even if the WebSocket endpoint sends an entire audio file, the synchronisation happens on **WSBrigde** side. Although the data is accepted, it is held in the network device queue.

Currently each packet received from the WebSocket connection is queued until the previously received packet is sent to the SIP/RTP side. However, note that this change affects the retrieval of data on the WebSocket connection only, and thus the circular buffer when writing data towards the WebSocket side is still in place.

_(docs written by: Tiago Lam, Dragos Oancea, Giacomo Vacca)_
