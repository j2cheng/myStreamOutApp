package com.crestron.streamout;

import java.util.Arrays;
import java.util.ArrayList;
import java.util.List;

import android.Manifest;
import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.util.Log;

import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;

public class RuntimePermission extends Activity
{
    public static String TAG = "strmout_RuntimePermActivity";
    public final static int REQUIRED_PERMISSIONS_REQUEST_CODE = 1000000;
    public final static int OVERLAY_PERMISSION_REQUEST_CODE = 1000001;

    private static final String[] requiredPermissions =
    {
        Manifest.permission.CAMERA
    };

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i(TAG, "onCreate");

        if(requestPermissions()) 
        {
            Log.w(TAG, "onCreate requestPermissions returned true");
            finish();
        }
        else
        {
            Log.w(TAG, "onCreate requestPermissions returned false, waiting for user response");            
        }
    }

    @Override
    protected void onStart()
    {
        Log.i(TAG, "super.onStart");
        super.onStart();
        Log.i(TAG, "onStart");
    }

    @Override
    protected void onStop()
    {
        Log.i(TAG, "super.onStop");
        super.onStop();
        Log.i(TAG, "onStop");
    }

    @Override
    protected void onRestart()
    {
        Log.i(TAG, "supert.onRestart");
        super.onRestart();
        Log.i(TAG, "onRestart");
    }

    @Override
    protected void onResume()
    {
        Log.i(TAG, "super.onResume");
        super.onResume();
        Log.i(TAG, "onResume");
    }

    // public void requestOverlayPermission()
    // {
    //     Log.i(TAG, "requestOverlayPermission");
    //     Intent intent = new Intent(
    //             Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
    //             Uri.parse("package:" + getApplicationContext().getPackageName()));
    //     startActivityForResult(intent, OVERLAY_PERMISSION_REQUEST_CODE);
    // }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data)
    {
        Log.i(TAG,
                "onActivityResult requestCode: " + String.valueOf(requestCode)
                + ", resultCode: " + String.valueOf(resultCode));
        if(requestCode == OVERLAY_PERMISSION_REQUEST_CODE) 
        {
            Log.w(TAG, "onActivityResult for overlay permission, resultCode: " + String.valueOf(resultCode));
            finish();
        }
        else
        {
            Log.w(TAG, "onActivityResult with unexpected requestCode: " + String.valueOf(requestCode));
        }
    }

    // protected void startServices()
    // {
    //     Log.i(TAG, "start CSS Service");
    //     startService(new Intent(this, CresStreamCtrl.class));
    //     //Log.i(TAG, "start CSIO Service");
    //     //startService(new Intent(this, CSIOService.class));
    //     Log.i(TAG, "startServices completed");
    // }


    private boolean requestPermissions()
    {
        List<String> needed = new ArrayList<>();

        for(String p: requiredPermissions)
        {
            if(
                PackageManager.PERMISSION_GRANTED
                == ContextCompat.checkSelfPermission(getApplicationContext(), p))
                continue;
            Log.i(TAG, "requesting: " + p);
            needed.add(p);
        }

        if(needed.isEmpty()) return true;

        ActivityCompat.requestPermissions(
                this,
                needed.toArray(new String[needed.size()]),
                REQUIRED_PERMISSIONS_REQUEST_CODE);
        return false;
    }

    @Override
    public void onRequestPermissionsResult(
            int requestCode, String permissions[], int[] results)
    {
        if(REQUIRED_PERMISSIONS_REQUEST_CODE != requestCode)
        {
            Log.w(TAG, "unexpected request code: " + requestCode);
            return;
        }

        Log.i(TAG, String.join(",", permissions) + ", " + Arrays.toString(results));
        boolean allGranted = true;

        for(int i = 0; i < results.length && i < permissions.length; ++i)
        {
            if(PackageManager.PERMISSION_GRANTED != results[i])
            {
                Log.e(TAG, "not granted: " + permissions[i]);
                allGranted = false;
            }
        }

        //if(allGranted) startServices();
        Log.w(TAG, "onRequestPermissionsResult allGranted: " + allGranted);

        // After Camera is handled, check if we need to request Overlay permission
        if (!Settings.canDrawOverlays(this)) {
            Log.i(TAG, "Camera handled, but Overlay permission still needed. Launching settings...");
            //requestOverlayPermission(); 
            // Do NOT call finish() here yet; we wait for onActivityResult
        } else {
            // Everything is granted (or denied), we are done.
            Log.i(TAG, "All permission flows complete. Finishing activity.");
            finish();
        }
    }
}
