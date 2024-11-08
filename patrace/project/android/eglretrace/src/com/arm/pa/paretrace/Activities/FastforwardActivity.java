package com.arm.pa.paretrace.Activities;

import android.app.Activity;
import android.content.Intent;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Message;
import android.util.Log;

import java.io.FileReader;
import java.io.BufferedReader;
import org.json.JSONObject;

import com.arm.pa.paretrace.R;

public class FastforwardActivity extends Activity {
    private static final String TAG = "paretrace FastforwardActivity";
    private static boolean isStarted = false;

    @Override
    public void onCreate(Bundle icicle) {
        super.onCreate(icicle);
        Log.d(TAG, "FastforwardActivity: onCreate()");
    }

    @Override
    protected void onStart() {
        super.onStart();
        Log.d(TAG, "FastforwardActivity: onStart()");
    }

    @Override
    protected void onResume() {
        super.onResume();
        Log.d(TAG, "FastforwardActivity: onResume()");

        if (!isStarted) {
            Intent receivedIntent = this.getIntent();

            // intent forward to retrace activity
            String input = receivedIntent.getStringExtra("input");
            String output = receivedIntent.getStringExtra("output");
            int targetFrame = receivedIntent.getIntExtra("targetFrame", -1);
            int endFrame = receivedIntent.getIntExtra("endFrame", -1);
            boolean multithread = receivedIntent.getBooleanExtra("multithread", false);
            boolean offscreen = receivedIntent.getBooleanExtra("offscreen", false);
            boolean noscreen = receivedIntent.getBooleanExtra("noscreen", false);
            boolean norestoretex = receivedIntent.getBooleanExtra("norestoretex", false);
            boolean version = receivedIntent.getBooleanExtra("version", false);
            int restorefbo0 = receivedIntent.getIntExtra("restorefbo0", -1);
            boolean shu = receivedIntent.getBooleanExtra("removeUnusedShader", true);
            boolean mipu = receivedIntent.getBooleanExtra("removeUnusedMipmap", true);
            boolean bfu = receivedIntent.getBooleanExtra("removeUnusedBuffer", true);
            boolean nsbfu = receivedIntent.getBooleanExtra("norestoreUnusedBuffer", true);
            boolean bfsd = receivedIntent.getBooleanExtra("removeBufferSubData", true);
            boolean txu = receivedIntent.getBooleanExtra("removeUnusedtex", false);
            boolean nstxu = receivedIntent.getBooleanExtra("norestoreUnusedtex", false);
            boolean txsi = receivedIntent.getBooleanExtra("removeTexSubImage", false);
            boolean cpimg = receivedIntent.getBooleanExtra("removeCopyImage", false);
            boolean bfmp = receivedIntent.getBooleanExtra("removeBufferMap", false);


            // init with json parameters
            if (receivedIntent.hasExtra("jsonData")) {
                String json_path = receivedIntent.getStringExtra("jsonData");
                Log.i(TAG, "json_path: " + json_path);
                String json_data = new String();

                // check if json_extra is a file path, and if so, read JSON data from the file
                if (json_path.charAt(0) == '/' || Character.isLetter(json_path.charAt(0))) {
                    try {
                        BufferedReader r = new BufferedReader(new FileReader(json_path));
                        StringBuilder total = new StringBuilder();
                        String line;
                        while ((line = r.readLine()) != null) {
                            total.append(line);
                        }
                        r.close();
                        json_data = total.toString();
                        Log.i(TAG, "json_data: " + json_data);
                        JSONObject json_Param = new JSONObject(json_data);
                        // intent has higher priority than jsonData
                        if (!receivedIntent.hasExtra("input") && json_Param.has("input")) {
                            input = json_Param.getString("input");
                        }
                        if (!receivedIntent.hasExtra("output") && json_Param.has("output")) {
                            output = json_Param.getString("output");
                        }
                        if (!receivedIntent.hasExtra("targetFrame") && json_Param.has("targetFrame")) {
                            targetFrame = json_Param.getInt("targetFrame");
                        }
                        if (!receivedIntent.hasExtra("endFrame") && json_Param.has("endFrame")) {
                            endFrame = json_Param.getInt("endFrame");
                        }
                        if (!receivedIntent.hasExtra("multithread") && json_Param.has("multithread")) {
                            multithread = json_Param.getBoolean("multithread");
                        }
                        if (!receivedIntent.hasExtra("offscreen") && json_Param.has("offscreen")) {
                            offscreen = json_Param.getBoolean("offscreen");
                        }
                        if (!receivedIntent.hasExtra("norestoretex") && json_Param.has("norestoretex")) {
                            norestoretex = json_Param.getBoolean("norestoretex");
                        }
                        if (!receivedIntent.hasExtra("version") && json_Param.has("version")) {
                            version = json_Param.getBoolean("version");
                        }
                        if (!receivedIntent.hasExtra("restorefbo0") && json_Param.has("restorefbo0")) {
                            restorefbo0 = json_Param.getInt("restorefbo0");
                        }
                        if (!receivedIntent.hasExtra("removeUnusedShader") && json_Param.has("removeUnusedShader")) {
                            shu = json_Param.getBoolean("removeUnusedShader");
                        }
                        if (!receivedIntent.hasExtra("removeUnusedMipmap") && json_Param.has("removeUnusedMipmap")) {
                            mipu = json_Param.getBoolean("removeUnusedMipmap");
                        }
                        if (!receivedIntent.hasExtra("removeUnusedBuffer") && json_Param.has("removeUnusedBuffer")) {
                            bfu = json_Param.getBoolean("removeUnusedBuffer");
                        }
                        if (!receivedIntent.hasExtra("norestoreUnusedBuffer") && json_Param.has("norestoreUnusedBuffer")) {
                            nsbfu = json_Param.getBoolean("norestoreUnusedBuffer");
                        }
                        if (!receivedIntent.hasExtra("removeBufferSubData") && json_Param.has("removeBufferSubData")) {
                            bfsd = json_Param.getBoolean("removeBufferSubData");
                        }
                        if (!receivedIntent.hasExtra("removeUnusedtex") && json_Param.has("removeUnusedtex")) {
                            txu = json_Param.getBoolean("removeUnusedtex");
                        }
                        if (!receivedIntent.hasExtra("norestoreUnusedtex") && json_Param.has("norestoreUnusedtex")) {
                            nstxu = json_Param.getBoolean("norestoreUnusedtex");
                        }
                        if (!receivedIntent.hasExtra("removeTexSubImage") && json_Param.has("removeTexSubImage")) {
                            txsi = json_Param.getBoolean("removeTexSubImage");
                        }
                        if (!receivedIntent.hasExtra("removeCopyImage") && json_Param.has("removeCopyImage")) {
                            cpimg = json_Param.getBoolean("removeCopyImage");
                        }
                        if (!receivedIntent.hasExtra("removeBufferMap") && json_Param.has("removeBufferMap")) {
                            bfmp = json_Param.getBoolean("removeBufferMap");
                        }
                    } catch (Exception e) {
                        json_data = ""; // shut up the compiler
                        e.printStackTrace();
                        System.exit(1);
                    }
                }
            }

            // combine the parameter into a string intent and forward to RetraceActivity
            String args = new String();
            if (input != null) {
                args += ("--input " + input + " ");
            }
            if (output != null) {
                args += ("--output " + output + " ");
            }
            if (targetFrame != -1) {
                args += ("--targetFrame " + targetFrame + " ");
            }
            if (endFrame != -1) {
                args += ("--endFrame " + endFrame + " ");
            }
            if (multithread == true) {
                args += "--multithread ";
            }
            if (offscreen == true) {
                args += "--offscreen ";
            }
            if (noscreen == true) {
                args += "--noscreen ";
            }
            if (norestoretex == true) {
                args += "--norestoretex ";
            }
            if (version == true) {
                args += "--version ";
            }
            if (restorefbo0 != -1) {
                args += ("--restorefbo0 " + restorefbo0 + " ");
            }

            if (shu == true) {
                args += "--removeUnusedShader 1 ";
            } else {
                args += "--removeUnusedShader 0 ";
            }

            if (mipu == true) {
                args += "--removeUnusedMipmap 1 ";
            } else {
                args += "--removeUnusedMipmap 0 ";
            }

            if (bfu == true) {
                args += "--removeUnusedBuffer 1 ";
            } else {
                args += "--removeUnusedBuffer 0 ";
            }

            if (nsbfu == true) {
                args += "--norestoreUnusedBuffer 1 ";
            } else {
                args += "--norestoreUnusedBuffer 0 ";
            }

            if (bfsd == true) {
                args += "--removeBufferSubData 1 ";
            } else {
                args += "--removeBufferSubData 0 ";
            }

            if (txu == true) {
                args += "--removeUnusedtex 1 ";
            } else {
                args += "--removeUnusedtex 0 ";
            }

            if (nstxu == true) {
                args += "--norestoreUnusedtex 1 ";
            } else {
                args += "--norestoreUnusedtex 0 ";
            }

            if (txsi == true) {
                args += "--removeTexSubImage 1 ";
            } else {
                args += "--removeTexSubImage 0 ";
            }

            if (cpimg == true) {
                args += "--removeCopyImage 1 ";
            } else {
                args += "--removeCopyImage 0 ";
            }

            if (bfmp == true) {
                args += "--removeBufferMap 1 ";
            } else {
                args += "--removeBufferMap 0 ";
            }

            Log.i(TAG, "forward args: " + args);

            Intent forwardIntent = new Intent(this, RetraceActivity.class);
            forwardIntent.putExtra("fastforward", args);
            this.startActivity(forwardIntent);
            isStarted = true;
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        Log.d(TAG, "FastforwardActivity: onPause()");
    }

    @Override
    protected void onStop() {
        super.onStop();
        Log.d(TAG, "FastforwardActivity: onStop()");
    }

    @Override
    public void onDestroy() {
        super.onDestroy();
        isStarted = false;
        Thread suicideThread = new Thread() {
            public void run() {
                try {
                    sleep(500);
                } catch (InterruptedException ie) {
                }
                System.exit(0);
            }
        };
        Log.d(TAG, "FastforwardActivity: onDestroy()");
        suicideThread.start();
    }
}
