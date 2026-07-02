package com.crestron.streamout;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

public class BootReceiver extends BroadcastReceiver
{
    public static String TAG = "strmout_BootReceiver";

    @Override
    public void onReceive(Context context, Intent intent)
    {
        if (Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction()))
        {
            //Note: 8-8-2024, calling to start an Activity somehow creates a 
            //      black window(focused com.crestron.txrxservice (previous: com.crestron.splash)).
            //      which covers Crestron splash window(with dots).
            //      So here we just bypass Activety, simply call txrx service(not Activity)

            // Intent activityIntent = new Intent(context, LaunchApp.class);
            // activityIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            // context.startActivity(activityIntent);
            Log.i(TAG, "BootReceiver.onReceive, start streamout Service");
            context.startForegroundService(new Intent(context, StreamOutSvcCtrl.class));
            Log.i(TAG, "BootReceiver.onReceive, startForegroundService completed");
        }
    }
}
