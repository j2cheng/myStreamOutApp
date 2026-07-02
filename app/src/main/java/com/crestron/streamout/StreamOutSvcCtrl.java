package com.crestron.streamout;

import android.app.Service;
import android.content.Intent;
import android.os.IBinder;
import android.util.Log;
import android.app.Notification;
import android.content.Context;
import org.freedesktop.gstreamer.GStreamer;
import android.os.Build;
import android.os.Handler;
import android.app.NotificationManager;
import android.app.NotificationChannel;
import android.app.NotificationChannel;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import java.util.List;
import java.io.File;
import java.io.FileInputStream;
import java.io.InputStreamReader;
import java.io.BufferedReader;
import org.json.JSONObject;

// permission helpers
import android.Manifest;
import android.content.pm.PackageManager;
import android.provider.Settings;
import androidx.core.content.ContextCompat;

//Android 14+: This is now strictly required. If you call startForeground , 
// need to specify the type,
// compileSdkVersion 34  Must be 29 or higher
import android.content.pm.ServiceInfo;

public class StreamOutSvcCtrl extends Service {

    static String TAG = "strmout_StreamOutSvcCtrl";
    // Singleton instance for JNI/gRPC access
    private static StreamOutSvcCtrl instance;

    public static final int GRPCPORT 	= 50051;        // Default gRPC grpcport

    //to save the path to the app's internal storage directory
    private static String myStoredPath;

    // 1 is just a common placeholder developers use.
    // 42 is likely a "magic number" or a reference to The Hitchhiker's Guide
    // to the Galaxy used by whoever wrote that specific part of the code.
    private static final int ONGOING_NOTIFICATION_ID = 42;

    public static StreamOutSvcCtrl getInstance() {
        return instance;
    }   

    static {
        Log.e(TAG, "build " + BuildConfig.BUILD_TIME + "==loadLibrary=native-lib===================");
        System.loadLibrary("native-lib");

        Log.e(TAG, "=====================loadLibrary exits===================");
    }

    private StrmOutGrpcServer grpcServer;
    private int grpcport  = GRPCPORT;     // Default gRPC grpcport
    private String grpcIp = "127.0.0.1";  // Default gRPC IP


    // private native int runNativeTests();
    // private native void nativeSetEnv(String key, String value);
    // JNI: Set Streamout port from Java
    // public native void nativeSetStreamoutPort(String port);
    // JNI: Set Streamout pipeline from Java
    // public native void nativeSetStreamoutPipeline(String pipeline);
    // JNI: Start Streamout from Java
    // public native void nativeStreamoutStart(int arg);
    // JNI: Stop Streamout from Java
    // public native void nativeStreamoutStop(int arg);

    // JNI: RTSP server debug command
    public native void jniRtspServerDebug(String cmdString);

    // JNI: Set product ID
    public native void nativeSetProductID(int id);

    // JNI: Initialize Streamout project with mode only once
    // public native void nativeStreamoutProjectInit(int mode);
    // public native void nativeStreamoutProjectDeInit();

    // Define a native method to receive the path
    // public native void setNativePath(String path);

    //came from eStreamoutMode in cresStreamOut.h, keep in sync
    public static final int STREAMOUT_MODE_CAMERA = 0;
    public static final int STREAMOUT_MODE_WIRELESSCONFERENCING = 1;
    public static final int STREAMOUT_MODE_CAMERA2_HWENC = 2;  // Camera2 API + MediaCodec HW encoding

    // Active streaming mode — only one mode at a time
    private int activeMode = STREAMOUT_MODE_CAMERA2_HWENC;  // default
    //private int activeMode = STREAMOUT_MODE_CAMERA; 
    private Camera2GstStreamer mCamera2Streamer;

    /**
     * Start Camera2+MediaCodec HW encoding path.
     * Stops any active V4L2 stream first (single-mode enforcement).
     */
    // public void camera2Start(String cameraId, int port, int width, int height,
    //                          int frameRate, int bitRate, boolean preferH265) {
    //     // Stop V4L2 pipeline if it was running
    //     if (activeMode != STREAMOUT_MODE_CAMERA2_HWENC) {
    //         nativeStreamoutStop(0);
    //     }
    //     if (mCamera2Streamer == null) {
    //         mCamera2Streamer = new Camera2GstStreamer(this);
    //     }
    //     Camera2GstStreamer.setPreferH265(preferH265);
    //     mCamera2Streamer.start(cameraId, "0.0.0.0", port, width, height, frameRate, bitRate);
    //     activeMode = STREAMOUT_MODE_CAMERA2_HWENC;
    //     Log.i(TAG, "camera2Start: cameraId=" + cameraId + " port=" + port
    //             + " " + width + "x" + height + "@" + frameRate + " bitRate=" + bitRate
    //             + " preferH265=" + preferH265);
    // }

    /**
     * Stop Camera2+MediaCodec HW encoding path.
     */
    public void camera2Stop() {
        if (mCamera2Streamer != null) {
            mCamera2Streamer.stop();
        }
        Log.i(TAG, "camera2Stop");
    }

    /**
     * Start Camera2 with auto-detected best camera (highest resolution).
     */
    // public void camera2StartBest(boolean preferH265) {
    //     if (activeMode != STREAMOUT_MODE_CAMERA2_HWENC) {
    //         nativeStreamoutStop(0);
    //     }
    //     if (mCamera2Streamer == null) {
    //         mCamera2Streamer = new Camera2GstStreamer(this);
    //     }
    //     Camera2GstStreamer.setPreferH265(preferH265);
    //     mCamera2Streamer.startBestCamera();
    //     activeMode = STREAMOUT_MODE_CAMERA2_HWENC;
    //     Log.i(TAG, "camera2StartBest: preferH265=" + preferH265);
    // }

    /**
     * Get the active streaming mode.
     */
    public int getActiveMode() {
        return activeMode;
    }

    /**
     * Switch the streaming mode. Stops the currently active stream first.
     * @param mode one of STREAMOUT_MODE_CAMERA, STREAMOUT_MODE_WIRELESSCONFERENCING, STREAMOUT_MODE_CAMERA2_HWENC
     */
    public void setMode(int mode) {
        if (mode == activeMode) {
            Log.i(TAG, "setMode: already in mode " + mode);
            return;
        }
        // Tear down current mode
        if (activeMode == STREAMOUT_MODE_CAMERA2_HWENC) {
            camera2Stop();
        } else {
            // nativeStreamoutStop(0);
        }
        // Always de-init and re-init native project for new mode
        // nativeStreamoutProjectDeInit();
        // nativeStreamoutProjectInit(mode);
        activeMode = mode;
        Log.i(TAG, "setMode: switched to mode " + mode + " (" + getModeName(mode) + ")");
    }

    /**
     * Get human-readable mode name.
     */
    public static String getModeName(int mode) {
        switch (mode) {
            case STREAMOUT_MODE_CAMERA:            return "camera";
            case STREAMOUT_MODE_WIRELESSCONFERENCING: return "wirelessconferencing";
            case STREAMOUT_MODE_CAMERA2_HWENC:     return "camera2_hwenc";
            default:                               return "unknown(" + mode + ")";
        }
    }

    /**
     * Unified start: routes to the correct pipeline based on activeMode.
     * For CAMERA2_HWENC, starts Camera2GstStreamer with best camera (auto-detect).
     * For V4L2 modes, calls into native streamout start.
     */
    public void startStreaming(int arg) {
        Log.i(TAG, "startStreaming: mode=" + activeMode + " (" + getModeName(activeMode) + ") arg=" + arg);
        if (activeMode == STREAMOUT_MODE_CAMERA2_HWENC) {
            if (mCamera2Streamer == null) {
                mCamera2Streamer = new Camera2GstStreamer(this);
            }
            mCamera2Streamer.startBestCamera();
            Log.i(TAG, "startStreaming: Camera2GstStreamer started (best camera, default settings)");
        } else {
            // nativeStreamoutStart(arg);
        }
    }

    /**
     * Unified stop: routes to the correct teardown based on activeMode.
     */
    public void stopStreaming(int arg) {
        Log.i(TAG, "stopStreaming: mode=" + activeMode + " (" + getModeName(activeMode) + ") arg=" + arg);
        if (activeMode == STREAMOUT_MODE_CAMERA2_HWENC) {
            camera2Stop();
        } else {
            // nativeStreamoutStop(arg);
        }
    }

    // Java wrapper for JNI call (optional, for clarity)
    public void jniRtspServerDebugWrapper(String cmdString) {
        Log.e(TAG, "jniRtspServerDebugWrappercalling jniRtspServerDebug with cmdString: " + cmdString);
        jniRtspServerDebug(cmdString);
    }

    private static final String NOTIFICATION_CHANNEL_ID = "ForegroundServiceChannel";

    private native int JNI_OnLoad();
    Handler handler;
    GstreamBase gstreamBase = null;

    public StreamOutSvcCtrl() {
        Log.e(TAG, "=====================constructor=================");
    }

    /**
     * Make sure the runtime permissions we need are satisfied.  If not, launch
     * the RuntimePermission activity from the supplied context so the user can
     * grant them.  This is safe to call from a Service (uses NEW_TASK flag).
     */
    private static void ensurePermissions(Context ctx) {
        boolean cameraGranted =
                ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA)
                        == PackageManager.PERMISSION_GRANTED;
        boolean audioGranted =
                ContextCompat.checkSelfPermission(ctx, Manifest.permission.RECORD_AUDIO)
                        == PackageManager.PERMISSION_GRANTED;
        boolean overlayGranted = Settings.canDrawOverlays(ctx);
        if (cameraGranted && audioGranted && overlayGranted) {

            Log.i(TAG, "ensurePermissions(): cameraGranted: " + cameraGranted
                    + ", audioGranted: " + audioGranted
                    + ", overlayGranted: " + overlayGranted);
            return;
        }

        Log.w(TAG, "ensurePermissions(): missing permissions:"
                + (!cameraGranted  ? " CAMERA"       : "")
                + (!audioGranted   ? " RECORD_AUDIO"  : "")
                + (!overlayGranted ? " OVERLAY"       : ""));
            
        // Intent i = new Intent(ctx, RuntimePermission.class);
        // i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        // ctx.startActivity(i);
    }

    public void runOnUiThread(Runnable runnable) {
        // Android wants all surface methods to be run on UI thread,
        // Instability and/or crashes can occur if this is not observed
        Log.i(TAG, "runOnUiThread: " + runnable.toString());
        handler.post(runnable);
    }
    
    private final Runnable foregroundRunnable = new Runnable() {
        @Override
        public void run() {
            Log.e(TAG, "=====================Service calling ForceServiceToForeground===================");
            ForceServiceToForeground();
            Log.e(TAG, "=====================Service ForceServiceToForeground ===================");
        }
    };

    public void RunNotificationThread()
    {
        new Thread(new Runnable() {
            public void run() {
                // In later versions of Android it is not necessary to continuously tell Android
                // that you want to be in the foreground
                if (Build.VERSION.SDK_INT >= 27 /*Build.VERSION_CODES.O*/)
                {
                    runOnUiThread(foregroundRunnable);
                }
                else
                {
                    while (true)
                    {
                        runOnUiThread(foregroundRunnable);
                        try
                        {
                            Thread.sleep(5000);
                        } catch (Exception e)
                        {
                            e.printStackTrace();
                        }
                    }
                }
            }
        }).start();
    }

    private void createNotificationChannel()
    {
        try{
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)
            {
                NotificationManager notificationManager =
                    getSystemService(NotificationManager.class);
                NotificationChannel channel =
                    new NotificationChannel(
                            NOTIFICATION_CHANNEL_ID,
                            NOTIFICATION_CHANNEL_ID /* name */,
                            NotificationManager.IMPORTANCE_DEFAULT);
    
                channel.setDescription("txrxservice notification channel");
                notificationManager.createNotificationChannel(channel);
            }
        }catch(Exception E){
            Log.e(TAG, "Exception: " + E.toString());
        }

    }
    /**
     * Force the service to the foreground
     */
    public void ForceServiceToForeground()
    {
        Notification.Builder builder =
            new Notification.Builder((Context)this, NOTIFICATION_CHANNEL_ID)
            .setContentTitle("GtestStreamOut")
            .setContentText("Running tests")
            .setSmallIcon(android.R.drawable.ic_dialog_info)
            .setWhen(System.currentTimeMillis());
        startForeground(ONGOING_NOTIFICATION_ID, 
                        builder.build(),
                        ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
                        | ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE);
        Log.i(TAG, "ForceServiceToForeground");
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        Log.i(TAG, "onStartCommand Service Started");
        // Ensure the service is promoted to foreground quickly to avoid
        // RemoteServiceException when started via startForegroundService().
        try {
            Notification notification;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                notification = new Notification.Builder(this, NOTIFICATION_CHANNEL_ID)
                        .setContentTitle("GtestStreamOut")
                        .setContentText("Running tests in background")
                        .setSmallIcon(android.R.drawable.ic_dialog_info)
                        .setWhen(System.currentTimeMillis())
                        .build();
            } else {
                notification = new Notification.Builder(this)
                        .setContentTitle("GtestStreamOut")
                        .setContentText("Running tests in background")
                        .setSmallIcon(android.R.drawable.ic_dialog_info)
                        .setWhen(System.currentTimeMillis())
                        .build();
            }
            startForeground(ONGOING_NOTIFICATION_ID, 
                            notification,
                            ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA
                             | ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE);
        } catch (Exception e) {
            Log.e(TAG, "Failed to start foreground: " + e.toString());
        }

        // insert code here to read gtest filter from intent 
        // and set environment variable before running tests
        if (intent != null) {
            Log.i(TAG, "Intent extras: " + intent.getExtras());

            // Set gRPC port from intent if provided
            if (intent.hasExtra("grpc_port")) {
                int intentPort = intent.getIntExtra("grpc_port", 50051);
                if (intentPort > 0 && intentPort < 65536) {
                    grpcport = intentPort;
                } else {
                    Log.w(TAG, "Invalid grpc_port in intent, using default 50051");
                    grpcport = GRPCPORT;
                }
            }
            // Set gRPC IP from intent if provided
            if (intent.hasExtra("grpc_ip")) {
                String intentIp = intent.getStringExtra("grpc_ip");
                if (intentIp != null && !intentIp.isEmpty()) {
                    grpcIp = intentIp;
                } else {
                    grpcIp = "127.0.0.1";
                }
            }
            String filter = intent.getStringExtra("gtest_filter");
            if (filter != null && !filter.isEmpty()) {
                // Set the environment variable from Java
                System.setProperty("GTEST_FILTER", filter);
                // OR if you have JNI access to setenv:
                // nativeSetEnv("GTEST_FILTER", filter);

                Log.i(TAG, "GTEST_FILTER set to: " + filter);
            }
            else 
            {
                Log.i(TAG, "No gtest_filter provided in intent, running all tests");
            }
        }
        else {
            // The service was killed and restarted by the system (START_STICKY)
            Log.i(TAG, "Service restarted automatically after exit/crash.");
            // Implement "cool-down" logic here if needed

            // 1. We know for sure the service was restarted by START_STICKY
            ActivityManager am = (ActivityManager) getSystemService(Context.ACTIVITY_SERVICE);
            List<ApplicationExitInfo> exitReasons = am.getHistoricalProcessExitReasons(null, 0, 1);

            if (!exitReasons.isEmpty()) {
                ApplicationExitInfo info = exitReasons.get(0);
                
                if (info.getReason() == ApplicationExitInfo.REASON_EXIT_SELF && info.getStatus() == 1) {
                    // 2. We confirm it was our C++ watchdog that triggered this
                    Log.w(TAG, "Recovering from camera deadlock (exit 1).");
                    
                    // Optional: Add a 2-3 second delay here before re-initializing 
                    // GStreamer to ensure the Camera HAL is fully "clean."
                    try {
                        Thread.sleep(3000);
                    } catch (InterruptedException e) {
                        e.printStackTrace();
                    }
                } else {
                    Log.w(TAG, "Service restarted, but exit reason was not self-triggered.");
                }
            }
        }

        // nativeStreamoutProjectInit(activeMode);
        // Log.i(TAG, "onStartCommand: initialized with mode=" + activeMode + " (" + getModeName(activeMode) + ")");

        // Running tests on a background thread so the Service doesn't block the UI thread
        // new Thread(() -> {
        //     int result = runNativeTests();
        //     Log.i(TAG, "onStartCommand Thread Tests finished with exit code: " + result);
        //     // Optional: Stop the service automatically after tests finish
        //     stopSelf();
        // }).start();

        // return START_NOT_STICKY;

        // Force the service to the foreground immediately to avoid 
        // RemoteServiceException when started via startForegroundService().
        boolean runGtest = false;
        if (intent != null && intent.hasExtra("run_gtest")) {
            runGtest = intent.getBooleanExtra("run_gtest", false);
        }

        //Note: 6-16-2026, runNativeTestsis for old streamout program, remove it from now on
        // if (runGtest) {
        //     new Thread(() -> {
        //         int result = runNativeTests();
        //         Log.i(TAG, "onStartCommand run_gtest is set, the exit code: " + result);
        //         // Optional: Stop the service automatically after tests finish
        //         //stopSelf();
        //     }).start();
        // }
        // else {
        //     Log.i(TAG, "onStartCommand: run_gtest flag not set, skipping native GTest");
        // }

        // Read config file for gRPC settings (overrides intent extras if present)
        loadGrpcConfig();

        loadPipelineConfig();
        
        // Start the gRPC server here, after reading grpcport and IP from intent/config
        if (grpcServer == null) {
            grpcServer = new StrmOutGrpcServer();
            final int finalGrpcPort = grpcport;
            final String finalGrpcIp = grpcIp;
            Log.e(TAG, "finalGrpcIp: " + finalGrpcIp + ", finalGrpcPort: " + finalGrpcPort);
            new Thread(() -> {
                try {
                    grpcServer.start(finalGrpcIp, finalGrpcPort);
                } catch (Exception e) {
                    Log.e(TAG, "Failed to start gRPC server", e);
                }
            }).start();
        }

        return START_STICKY;
    }

    /**
     * Read gRPC IP and port from configure.txt in the app's internal storage directory.
     * Expected JSON format: {
     *                          "grpc_ip": "127.0.0.1",
     *                          "grpc_port": 50051,
     *                          "streamout_mode": 0,
     *                          "rtcp_feedback": false,
     *                          "prefer_h265": false,
     *                          "max_bitrate": 0,
     *                          "stream_mode": 0
     *                       }
     * streamout_mode: calls setMode() only when value is 0 (STREAMOUT_MODE_CAMERA).
     * rtcp_feedback: advertise a=rtcp-fb (nack/pli/fir) in the RTSP SDP; takes
     *                effect on the next stream start.
     * prefer_h265:   select H.265 (true) or H.264 (false) encoding; takes effect
     *                on the next stream start.
     * max_bitrate:   upper bound on the encoder bitrate in bps (0 = no cap); takes
     *                effect on the next stream start.
     * stream_mode:   which media streams to serve (0 = VIDEO_ONLY, 1 = AUDIO_ONLY,
     *                2 = VIDEO_AND_AUDIO_BOTH); takes effect on the next stream start.
     * Values from this file take precedence over intent extras (since it runs after them).
     */
    private void loadGrpcConfig() {
        File configFile = new File(getFilesDir(), "configure.txt");
        if (!configFile.exists()) {
            Log.i(TAG, "loadGrpcConfig: configure.txt not found at " + configFile.getAbsolutePath());
            return;
        }
        try {
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(new FileInputStream(configFile)));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line);
            }
            reader.close();

            JSONObject json = new JSONObject(sb.toString());

            if (json.has("grpc_ip")) {
                String ip = json.getString("grpc_ip");
                if (ip != null && !ip.isEmpty()) {
                    grpcIp = ip;
                    Log.i(TAG, "loadGrpcConfig: grpc_ip set to " + grpcIp);
                }
            }
            if (json.has("grpc_port")) {
                int port = json.getInt("grpc_port");
                if (port > 0 && port < 65536) {
                    grpcport = port;
                    Log.i(TAG, "loadGrpcConfig: grpc_port set to " + grpcport);
                }
            }
            if (json.has("streamout_mode")) {
                int mode = json.getInt("streamout_mode");
                if (mode == STREAMOUT_MODE_CAMERA) {
                    setMode(mode);
                    Log.i(TAG, "loadGrpcConfig: streamout_mode set to " + mode);
                }
            }
            if (json.has("rtcp_feedback")) {
                boolean rtcpFeedback = json.getBoolean("rtcp_feedback");
                Camera2GstStreamer.setRtcpFeedback(rtcpFeedback);
                Log.i(TAG, "loadGrpcConfig: rtcp_feedback set to " + rtcpFeedback);
            }
            if (json.has("prefer_h265")) {
                boolean preferH265 = json.getBoolean("prefer_h265");
                Camera2GstStreamer.setPreferH265(preferH265);
                Log.i(TAG, "loadGrpcConfig: prefer_h265 set to " + preferH265);
            }
            if (json.has("max_bitrate")) {
                int maxBitrate = json.getInt("max_bitrate");
                Camera2GstStreamer.setMaxBitRate(maxBitrate);
                Log.i(TAG, "loadGrpcConfig: max_bitrate set to " + maxBitrate);
            }
            if (json.has("stream_mode")) {
                int streamMode = json.getInt("stream_mode");
                Camera2GstStreamer.setStreamMode(streamMode);
                Log.i(TAG, "loadGrpcConfig: stream_mode set to " + streamMode);
            }
        } catch (Exception e) {
            Log.e(TAG, "loadGrpcConfig: failed to read configure.txt", e);
        }
    }

    /**
     * Read the RTSP pipeline string from rtsp_server_pipeline in the app's internal
     * storage directory.  If the file exists its content is passed to
     * nativeSetStreamoutPipeline(); otherwise an empty string is set.
     */
    private void loadPipelineConfig() {
        File pipelineFile = new File(getFilesDir(), "rtsp_server_pipeline");
        if (!pipelineFile.exists()) {
            Log.i(TAG, "loadPipelineConfig: rtsp_server_pipeline not found at "
                    + pipelineFile.getAbsolutePath() + " – setting empty pipeline");
            setCustomeStreamoutPipeline("");
            return;
        }
        try {
            BufferedReader reader = new BufferedReader(
                    new InputStreamReader(new FileInputStream(pipelineFile)));
            StringBuilder sb = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                sb.append(line);
            }
            reader.close();

            String pipeline = sb.toString().trim();
            setCustomeStreamoutPipeline(pipeline);
            Log.i(TAG, "loadPipelineConfig: pipeline set to \"" + pipeline + "\"");
        } catch (Exception e) {
            Log.e(TAG, "loadPipelineConfig: failed to read rtsp_server_pipeline", e);
            setCustomeStreamoutPipeline("");
        }
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onCreate() {
        instance = this;
        super.onCreate();

        Log.i(TAG, "onCreate Service Started - Running GStreamer ...");

        createNotificationChannel();

        handler = new Handler();

        RunNotificationThread();

        try {
            // Set the debug level (e.g., 4 for DEBUG, 5 for LOG)
            android.system.Os.setenv("GST_DEBUG", "2", true); 
            // Optional: Disable ANSI colors for cleaner Logcat output
            android.system.Os.setenv("GST_DEBUG_NO_COLOR", "1", true);
        } catch (android.system.ErrnoException e) {
            e.printStackTrace();
        }

        
        // if we don't already have camera/overlay permission, prompt the user
        ensurePermissions(this);
        
        // Create GstreamBase first! GstreamBase will load gstreamer_android and 
        // initialize GStreamer. This is required before calling any 
        // GStreamer methods, including GStreamer.init()
        // Note: (GStreamer.init()->nativeInit->gst_android_init_jni->gst_android_init->gst_init_check->init_post)

        final CountDownLatch latch = new CountDownLatch(1);
        Thread startGstreamerThread = new Thread(new Runnable() {
            public void run() {
                gstreamBase = new GstreamBase(StreamOutSvcCtrl.this);
                latch.countDown();
            }
        });
        startGstreamerThread.start();

        boolean successfulStart = true; //indicates that there was no time out condition
        try { successfulStart = latch.await(3000, TimeUnit.MILLISECONDS); }
        catch (InterruptedException ex) { ex.printStackTrace(); }

        // Library failed to load kill mediaserver and restart txrxservice
        if (!successfulStart)
        {
            Log.e(TAG, "=====================OnCreate Gstreamer failed to initialize, restarting ???===================");
        }
        else
        {
            Log.e(TAG, "=====================OnCreate Gstreamer successfully initialized===================");
        }

        // Register camera availability callback after GStreamer init is complete
        // so all native libraries are loaded before we interact with Camera2.
        Camera2GstStreamer.registerCameraAvailability(this);

        // App-internal file -- writable without extra permissions
        myStoredPath = getFilesDir().getAbsolutePath();
        //this is for gtest only
        // nativeSetEnv("RTSP_PIPELINE_PATH", myStoredPath);

        //this is for streamoutsvc to find the location
        // setNativePath(myStoredPath);

        // move this part to onStartCommand after reading intent extras for gRPC configuration
        // Start the gRPC server on a background thread (example address: 0.0.0.0:50051)        
        //startGrpcServer("0.0.0.0:50051");
        // grpcServer = new StrmOutGrpcServer();
        // final int finalGrpcPort = grpcport;
        // final String finalGrpcIp = grpcIp;
        // Log.e(TAG, "finalGrpcIp: " + finalGrpcIp + ", finalGrpcPort: " + finalGrpcPort);
        // new Thread(() -> {
        //     try {
        //         grpcServer.start(finalGrpcIp, finalGrpcPort);
        //     } catch (Exception e) {
        //         Log.e(TAG, "Failed to start gRPC server", e);
        //     }
        // }).start();     
        
        
        //jniRtspServerDebugWrapper("STREAMOUT INSPECT_ELEMENT amcvidenc-omxqcomvideoencoderhevc");
    
        //jniRtspServerDebugWrapper("STREAMOUT INSPECT_ELEMENT tinyalsasrc");    
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
            if (grpcServer != null) {
                grpcServer.stop();
            }

            // Stop Camera2 streamer if active
            if (mCamera2Streamer != null) {
                mCamera2Streamer.stop();
                mCamera2Streamer = null;
            }

            // nativeStreamoutProjectDeInit();
    }

    // Static method to allow native code to access the stored path 
    public static String getMyStoredPath() {
        return myStoredPath;
    }   

    public void setCustomeStreamoutPipeline(String pipeline) {
        // nativeSetStreamoutPipeline(pipeline);
        Camera2GstStreamer.setCustomPipeline(pipeline);

        Log.i(TAG, "setCustomeStreamoutPipeline is called with pipeline: " + pipeline);
    }  
}
