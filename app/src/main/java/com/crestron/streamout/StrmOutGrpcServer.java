package com.crestron.streamout;

import io.grpc.Server;
import android.util.Log;
import io.grpc.netty.NettyServerBuilder;
import io.grpc.stub.StreamObserver;
import io.grpc.stub.ServerCallStreamObserver;
import java.io.IOException;
import java.util.concurrent.TimeUnit;

import streamout.v1.StreamoutServiceGrpc;
import streamout.v1.GrpcStrOut.HelloRequest;
import streamout.v1.GrpcStrOut.HelloReply;
import streamout.v1.GrpcStrOut.StreamoutSetPortRequest;
import streamout.v1.GrpcStrOut.StreamoutSetPipelineRequest;
import streamout.v1.GrpcStrOut.StreamoutStartRequest;
import streamout.v1.GrpcStrOut.StreamoutStopRequest;
import streamout.v1.GrpcStrOut.StreamoutSetProductIdRequest;
import streamout.v1.GrpcStrOut.StreamoutActionResponse;
import streamout.v1.GrpcStrOut.StreamoutDebugRequest;
import streamout.v1.GrpcStrOut.StreamoutStatusRequest;
import streamout.v1.GrpcStrOut.StreamoutStatusResponse;
import streamout.v1.GrpcStrOut.StreamStatus;

import com.crestron.streamout.StreamOutSvcCtrl;

public class StrmOutGrpcServer {
    // List to keep track of observers for StreamoutWatchStatus
    private static final java.util.concurrent.CopyOnWriteArrayList<StreamObserver<StreamoutStatusResponse>> streamStatusObservers = new java.util.concurrent.CopyOnWriteArrayList<>();

    // Last reported status per stream id, so a freshly (re)connecting
    // StreamoutWatchStatus subscriber can be primed with the current state
    // immediately instead of waiting for the next change.  When a gRPC
    // connection drops and the client reconnects it would otherwise observe
    // nothing until the next streamStatusFromNative() call; this cache closes
    // that gap.  The initial state for an unknown id is STREAM_STATUS_UNSPECIFIED.
    private static final java.util.Map<Integer, StreamoutStatusResponse> lastStatusById = new java.util.HashMap<>();

    // Guards (cache update + broadcast) against (observer registration +
    // snapshot) so a status change cannot interleave between a new subscriber
    // being primed and being registered, which would let it see a stale status
    // AFTER the live one.
    private static final Object statusLock = new Object();

    // Build the snapshot to send to a subscriber for the given id: the last
    // known status, or STREAM_STATUS_UNSPECIFIED if none has been reported yet.
    // Caller must hold statusLock.
    private static StreamoutStatusResponse currentStatusForId(int streamId) {
        StreamoutStatusResponse cached = lastStatusById.get(streamId);
        if (cached != null) {
            return cached;
        }
        return StreamoutStatusResponse.newBuilder()
                .setStreamId(streamId)
                .setStatusCode(StreamStatus.STREAM_STATUS_UNSPECIFIED)
                .setStatusInfo("No status reported yet")
                .build();
    }

    // JNI-accessible static method for native code to call to notify all StreamoutWatchStatus observers
    public static void streamStatusFromNative(int streamId, StreamStatus statusCode, String info) {
        StreamoutStatusResponse msg = StreamoutStatusResponse.newBuilder()
                .setStreamId(streamId)
                .setStatusCode(statusCode)
                .setStatusInfo(info != null ? info : "")
                .build();
        synchronized (statusLock) {
            // Remember the latest status so future subscribers can be primed.
            lastStatusById.put(streamId, msg);
            Log.i(TAG, "streamStatusFromNative: id=" + streamId + " code=" + statusCode
                    + " -> notifying " + streamStatusObservers.size() + " clients");
            for (StreamObserver<StreamoutStatusResponse> observer : streamStatusObservers) {
                try {
                    observer.onNext(msg);
                } catch (Exception e) {
                    Log.e(TAG, "streamStatusFromNative: Error sending message", e);
                    streamStatusObservers.remove(observer);
                }
            }
        }
    }

    // Log tag for this class
    private static final String TAG = "strmout_StrmOutGrpcServer";

    // Start the gRPC server
    public void start(String ip, int port) throws IOException {
        server = NettyServerBuilder.forAddress(new java.net.InetSocketAddress(ip, port))
                .addService(new StreamoutServiceImpl())

                // 1. Allow pings even if there are no active RPC calls
                .permitKeepAliveWithoutCalls(true)

                // 2. Set the minimum allowed time between pings from the client.
                .permitKeepAliveTime(1, TimeUnit.SECONDS)

                .build()
                .start();
    }

    // Stop the gRPC server
    public void stop() {
        if (server != null) {
            server.shutdown();
        }
    }

    // Static method for JNI to call shutdown
    public static void shutdownGrpcServer() {
        Log.i(TAG, "shutdownGrpcServer called from native");
    }

    private Server server;

    // Implement the gRPC service
    static class StreamoutServiceImpl extends StreamoutServiceGrpc.StreamoutServiceImplBase {
        @Override
        public void sayHello(HelloRequest request, StreamObserver<HelloReply> responseObserver) {
            Log.i(TAG, "sayHello request received: name=" + request.getName());
            HelloReply reply = HelloReply.newBuilder()
                    .setMessage("Hello, " + request.getName())
                    .build();
            try {
                responseObserver.onNext(reply);
                responseObserver.onCompleted();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        @Override
        public void streamoutSetPort(StreamoutSetPortRequest request, StreamObserver<StreamoutActionResponse> responseObserver) {
            Log.i(TAG, "streamoutSetPort request received: port=" + request.getPort());
            String port = request.getPort();
            boolean success = false;
            String message;
            try {
                StreamOutSvcCtrl svc = StreamOutSvcCtrl.getInstance();
                if (svc != null) {
                    // svc.nativeSetStreamoutPort(port);
                    success = true;
                    message = "Port set: " + port;
                } else {
                    message = "Service instance not available";
                }
            } catch (Exception e) {
                message = "Exception: " + e.getMessage();
            }
            StreamoutActionResponse response = StreamoutActionResponse.newBuilder()
                    .setSuccess(success)
                    .setMessage(message)
                    .build();
            try {
                responseObserver.onNext(response);
                responseObserver.onCompleted();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        @Override
        public void streamoutSetPipeline(StreamoutSetPipelineRequest request, StreamObserver<StreamoutActionResponse> responseObserver) {
            Log.i(TAG, "streamoutSetPipeline request received: pipeline=" + request.getPipeline());
            String pipeline = request.getPipeline();
            boolean success = false;
            String message;
            try {
                StreamOutSvcCtrl svc = StreamOutSvcCtrl.getInstance();
                if (svc != null) {
                    svc.setCustomeStreamoutPipeline(pipeline);
                    success = true;
                    message = "Pipeline set: " + pipeline;
                } else {
                    message = "Service instance not available";
                }
            } catch (Exception e) {
                message = "Exception: " + e.getMessage();
            }
            StreamoutActionResponse response = StreamoutActionResponse.newBuilder()
                    .setSuccess(success)
                    .setMessage(message)
                    .build();
            try {
                responseObserver.onNext(response);
                responseObserver.onCompleted();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        @Override
        public void streamoutServerDebug(StreamoutDebugRequest request, StreamObserver<StreamoutActionResponse> responseObserver) {
            Log.i(TAG, "streamoutServerDebug request received: debugString=" + request.getDebugString());
            String debugString = request.getDebugString();
            boolean success = false;
            String message;
            try {
                StreamOutSvcCtrl svc = StreamOutSvcCtrl.getInstance();
                if (svc != null) {
                    svc.jniRtspServerDebugWrapper(debugString);
                    success = true;
                    message = "Called debug with: " + debugString;
                } else {
                    message = "Service instance not available";
                }
            } catch (Exception e) {
                message = "Exception: " + e.getMessage();
            }
            StreamoutActionResponse response = StreamoutActionResponse.newBuilder()
                    .setSuccess(success)
                    .setMessage(message)
                    .build();
            try {
                responseObserver.onNext(response);
                responseObserver.onCompleted();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        @Override
        public void streamoutStart(StreamoutStartRequest request, StreamObserver<StreamoutActionResponse> responseObserver) {
            Log.i(TAG, "streamoutStart request received: arg=" + request.getArg());
            int arg = request.getArg();
            boolean success = false;
            String message;
            try {
                StreamOutSvcCtrl svc = StreamOutSvcCtrl.getInstance();
                if (svc != null) {
                    svc.startStreaming(arg);
                    success = true;
                    message = "Started with arg: " + arg + " mode: " + StreamOutSvcCtrl.getModeName(svc.getActiveMode());
                } else {
                    message = "Service instance not available";
                }
            } catch (Exception e) {
                message = "Exception: " + e.getMessage();
            }
            StreamoutActionResponse response = StreamoutActionResponse.newBuilder()
                    .setSuccess(success)
                    .setMessage(message)
                    .build();
            try {
                responseObserver.onNext(response);
                responseObserver.onCompleted();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        @Override
        public void streamoutStop(StreamoutStopRequest request, StreamObserver<StreamoutActionResponse> responseObserver) {
            Log.i(TAG, "streamoutStop request received: arg=" + request.getArg());
            int arg = request.getArg();
            boolean success = false;
            String message;
            try {
                StreamOutSvcCtrl svc = StreamOutSvcCtrl.getInstance();
                if (svc != null) {
                    svc.stopStreaming(arg);
                    success = true;
                    message = "Stopped with arg: " + arg;
                } else {
                    message = "Service instance not available";
                }
            } catch (Exception e) {
                message = "Exception: " + e.getMessage();
            }
            StreamoutActionResponse response = StreamoutActionResponse.newBuilder()
                    .setSuccess(success)
                    .setMessage(message)
                    .build();
            try {
                responseObserver.onNext(response);
                responseObserver.onCompleted();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        @Override
        public void streamoutSetProductId(StreamoutSetProductIdRequest request, StreamObserver<StreamoutActionResponse> responseObserver) {
            Log.i(TAG, "streamoutSetProductId request received: id=" + request.getId());
            int id = request.getId();
            boolean success = false;
            String message;
            try {
                StreamOutSvcCtrl svc = StreamOutSvcCtrl.getInstance();
                if (svc != null) {
                    svc.nativeSetProductID(id);
                    success = true;
                    message = "ProductID set to: " + id;
                } else {
                    message = "Service instance not available";
                }
            } catch (Exception e) {
                message = "Exception: " + e.getMessage();
            }
            StreamoutActionResponse response = StreamoutActionResponse.newBuilder()
                    .setSuccess(success)
                    .setMessage(message)
                    .build();
            try {
                responseObserver.onNext(response);
                responseObserver.onCompleted();
            } catch (Exception e) {
                e.printStackTrace();
            }
        }

        @Override
        public void streamoutWatchStatus(StreamoutStatusRequest request,
                StreamObserver<StreamoutStatusResponse> responseObserver) {
            Log.i(TAG, "streamoutWatchStatus subscription registered, id=" + request.getId());

            // Cast to ServerCallStreamObserver to detect client cancellation
            ServerCallStreamObserver<StreamoutStatusResponse> serverObserver =
                    (ServerCallStreamObserver<StreamoutStatusResponse>) responseObserver;

            StreamObserver<StreamoutStatusResponse> wrappedObserver = new StreamObserver<StreamoutStatusResponse>() {
                @Override
                public void onNext(StreamoutStatusResponse msg) {
                    try {
                        if (!serverObserver.isCancelled()) {
                            responseObserver.onNext(msg);
                        } else {
                            Log.i(TAG, "streamoutWatchStatus: Client already cancelled, removing observer");
                            streamStatusObservers.remove(this);
                        }
                    } catch (Exception e) {
                        Log.e(TAG, "streamoutWatchStatus: Error sending message", e);
                        streamStatusObservers.remove(this);
                    }
                }

                @Override
                public void onError(Throwable t) {
                    Log.i(TAG, "streamoutWatchStatus: Client cancelled or error occurred");
                    streamStatusObservers.remove(this);
                }

                @Override
                public void onCompleted() {
                    Log.i(TAG, "streamoutWatchStatus: Stream completed");
                    streamStatusObservers.remove(this);
                    responseObserver.onCompleted();
                }
            };

            // Register cancellation handler to remove observer when client disconnects
            serverObserver.setOnCancelHandler(() -> {
                Log.i(TAG, "streamoutWatchStatus: Client cancelled (onCancelHandler)");
                streamStatusObservers.remove(wrappedObserver);
            });

            // Register the observer and immediately prime it with the current
            // status for the requested id.  Done under statusLock so a status
            // broadcast cannot interleave between the snapshot and registration
            // (which would let the client observe the live update first and the
            // stale snapshot second).  If nothing has been reported yet the
            // client receives STREAM_STATUS_UNSPECIFIED as its initial state.
            synchronized (statusLock) {
                streamStatusObservers.add(wrappedObserver);
                StreamoutStatusResponse snapshot = currentStatusForId(request.getId());
                Log.i(TAG, "streamoutWatchStatus: priming new subscriber id=" + request.getId()
                        + " with code=" + snapshot.getStatusCode());
                wrappedObserver.onNext(snapshot);
            }
        }
    } // end of StreamoutServiceImpl

} // end of StrmOutGrpcServer
