package net.onmydisk.app;

import android.app.Activity;
import android.os.CancellationSignal;
import android.util.Log;

import androidx.annotation.NonNull;
import androidx.core.content.ContextCompat;
import androidx.credentials.Credential;
import androidx.credentials.CredentialManager;
import androidx.credentials.CredentialManagerCallback;
import androidx.credentials.CustomCredential;
import androidx.credentials.GetCredentialRequest;
import androidx.credentials.GetCredentialResponse;
import androidx.credentials.exceptions.GetCredentialCancellationException;
import androidx.credentials.exceptions.GetCredentialException;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;
import com.google.android.libraries.identity.googleid.GetSignInWithGoogleOption;
import com.google.android.libraries.identity.googleid.GoogleIdTokenCredential;

@CapacitorPlugin(name = "NativeGoogleAuth")
public class NativeGoogleAuthPlugin extends Plugin {
    private static final String TAG = "NativeGoogleAuth";

    @PluginMethod
    public void signIn(PluginCall call) {
        final String webClientId = call.getString("webClientId", "");
        if (webClientId == null || webClientId.trim().isEmpty()) {
            call.reject("Missing webClientId", "INVALID_ARGUMENT");
            return;
        }

        final Activity activity = getActivity();
        if (activity == null || activity.isFinishing()) {
            call.reject("Activity unavailable", "NO_ACTIVITY");
            return;
        }

        activity.runOnUiThread(() -> {
            try {
                CredentialManager credentialManager = CredentialManager.create(activity);
                GetSignInWithGoogleOption signInOption = new GetSignInWithGoogleOption.Builder(webClientId.trim())
                    .build();
                GetCredentialRequest request = new GetCredentialRequest.Builder()
                    .addCredentialOption(signInOption)
                    .build();

                CancellationSignal cancellationSignal = new CancellationSignal();
                Log.i(TAG, "Launching Android CredentialManager Sign in with Google sheet");

                credentialManager.getCredentialAsync(
                    activity,
                    request,
                    cancellationSignal,
                    ContextCompat.getMainExecutor(activity),
                    new CredentialManagerCallback<GetCredentialResponse, GetCredentialException>() {
                        @Override
                        public void onResult(GetCredentialResponse result) {
                            try {
                                Credential credential = result.getCredential();
                                if (credential instanceof CustomCredential &&
                                    GoogleIdTokenCredential.TYPE_GOOGLE_ID_TOKEN_CREDENTIAL.equals(credential.getType())) {
                                    GoogleIdTokenCredential googleCred = GoogleIdTokenCredential.createFrom(credential.getData());
                                    String idToken = googleCred.getIdToken();
                                    if (idToken == null || idToken.isEmpty()) {
                                        call.reject("Empty Google ID token", "EMPTY_TOKEN");
                                        return;
                                    }
                                    JSObject ret = new JSObject();
                                    ret.put("idToken", idToken);
                                    ret.put("email", googleCred.getId());
                                    if (googleCred.getDisplayName() != null) {
                                        ret.put("name", googleCred.getDisplayName());
                                    }
                                    if (googleCred.getProfilePictureUri() != null) {
                                        ret.put("imageUrl", googleCred.getProfilePictureUri().toString());
                                    }
                                    Log.i(TAG, "Google CredentialManager sign-in succeeded for: " + googleCred.getId());
                                    call.resolve(ret);
                                } else {
                                    Log.e(TAG, "Unexpected credential type: " + (credential != null ? credential.getType() : "null"));
                                    call.reject("Unexpected credential type", "UNEXPECTED_CREDENTIAL");
                                }
                            } catch (Exception ex) {
                                Log.e(TAG, "Failed to parse GoogleIdTokenCredential", ex);
                                call.reject("Failed to parse Google credential: " + ex.getMessage(), "PARSE_ERROR", ex);
                            }
                        }

                        @Override
                        public void onError(@NonNull GetCredentialException e) {
                            if (e instanceof GetCredentialCancellationException) {
                                Log.i(TAG, "User canceled Google Sign-In sheet");
                                call.reject("User canceled Google Sign-In", "USER_CANCELED", e);
                                return;
                            }
                            Log.e(TAG, "CredentialManager Google Sign-In error [" + e.getType() + "]: " + e.getMessage(), e);
                            call.reject(
                                e.getMessage() != null ? e.getMessage() : "Google Sign-In failed",
                                e.getType(),
                                e
                            );
                        }
                    }
                );
            } catch (Exception e) {
                Log.e(TAG, "Exception starting CredentialManager", e);
                call.reject("Failed to launch Google Sign-In: " + e.getMessage(), "LAUNCH_ERROR", e);
            }
        });
    }
}

