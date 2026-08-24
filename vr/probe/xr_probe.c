/*
xr_probe.c - OpenXR runtime capability probe for the Xash3D FWGS PCVR fork

Purpose: answer, empirically and for THIS machine, the questions that decide the
whole rendering approach:

  1. Which OpenXR runtime actually answers a 32-bit process?
  2. Does it expose XR_KHR_opengl_enable (can we submit OpenGL directly)?
  3. If not, does it expose XR_KHR_D3D11_enable (do we need GL->D3D11 interop)?
  4. What view configuration / eye resolution does it report?

Build (32-bit, to match the engine):  see build_probe.bat in this directory.

This is a diagnostic tool, not engine code. It links only the OpenXR loader.
*/

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static const char *result_str( XrInstance inst, XrResult r )
{
	static char buf[XR_MAX_RESULT_STRING_SIZE];
	if( inst != XR_NULL_HANDLE && XR_SUCCEEDED( xrResultToString( inst, r, buf )))
		return buf;
	snprintf( buf, sizeof( buf ), "XrResult(%d)", (int)r );
	return buf;
}

int main( void )
{
	XrResult          res;
	uint32_t          count = 0, i;
	XrExtensionProperties *exts = NULL;
	int               have_gl = 0, have_gl_es = 0, have_d3d11 = 0, have_d3d12 = 0, have_vulkan = 0;

	printf( "=====================================================\n" );
	printf( " OpenXR probe  (process is %d-bit)\n", (int)( sizeof( void * ) * 8 ));
	printf( "=====================================================\n\n" );

	/* ---- 1. API layers ---- */
	count = 0;
	res = xrEnumerateApiLayerProperties( 0, &count, NULL );
	printf( "[api layers] count=%u (%s)\n", count, result_str( XR_NULL_HANDLE, res ));

	/* ---- 2. Instance extensions ---- */
	count = 0;
	res = xrEnumerateInstanceExtensionProperties( NULL, 0, &count, NULL );
	if( XR_FAILED( res ))
	{
		printf( "\nFATAL: xrEnumerateInstanceExtensionProperties failed: %s\n",
			result_str( XR_NULL_HANDLE, res ));
		printf( "  -> No usable OpenXR runtime answered this process.\n" );
		printf( "  -> For a 32-bit process the runtime must be registered under\n" );
		printf( "     HKLM\\SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1\\ActiveRuntime\n" );
		return 1;
	}

	exts = (XrExtensionProperties *)calloc( count, sizeof( XrExtensionProperties ));
	for( i = 0; i < count; i++ )
		exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;

	res = xrEnumerateInstanceExtensionProperties( NULL, count, &count, exts );
	printf( "\n[instance extensions] %u supported:\n", count );
	for( i = 0; i < count; i++ )
	{
		printf( "    %-52s v%u\n", exts[i].extensionName, exts[i].extensionVersion );
		if( !strcmp( exts[i].extensionName, XR_KHR_OPENGL_ENABLE_EXTENSION_NAME ))    have_gl     = 1;
		if( !strcmp( exts[i].extensionName, "XR_KHR_opengl_es_enable" ))              have_gl_es  = 1;
		if( !strcmp( exts[i].extensionName, "XR_KHR_D3D11_enable" ))                  have_d3d11  = 1;
		if( !strcmp( exts[i].extensionName, "XR_KHR_D3D12_enable" ))                  have_d3d12  = 1;
		if( !strcmp( exts[i].extensionName, "XR_KHR_vulkan_enable" ))                 have_vulkan = 1;
	}

	printf( "\n---------------- GRAPHICS BINDING VERDICT ----------------\n" );
	printf( "  XR_KHR_opengl_enable : %s   <-- what Xash's ref_gl wants\n", have_gl     ? "YES" : "no" );
	printf( "  XR_KHR_D3D11_enable  : %s   <-- interop fallback path\n",    have_d3d11  ? "YES" : "no" );
	printf( "  XR_KHR_D3D12_enable  : %s\n",                               have_d3d12  ? "YES" : "no" );
	printf( "  XR_KHR_vulkan_enable : %s\n",                               have_vulkan ? "YES" : "no" );
	printf( "  XR_KHR_opengl_es     : %s\n",                               have_gl_es  ? "YES" : "no" );
	printf( "----------------------------------------------------------\n" );

	if( have_gl )
		printf( "  => DIRECT OpenGL submission is available. Best case.\n" );
	else if( have_d3d11 )
		printf( "  => No GL binding. Need GL->D3D11 interop (WGL_NV_DX_interop2).\n" );
	else
		printf( "  => NEITHER. Serious problem - investigate runtime choice.\n" );
	printf( "----------------------------------------------------------\n\n" );

	/* ---- 3. Create an instance so we can name the runtime ----
	 *
	 * IMPORTANT: do NOT blindly request XR_CURRENT_API_VERSION. The Khronos SDK
	 * headers advertise 1.1.x, but many shipping runtimes (VirtualDesktopXR's
	 * 32-bit runtime included) only implement OpenXR 1.0 and will reject the
	 * instance with XR_ERROR_API_VERSION_UNSUPPORTED (-4). Negotiate downward.
	 */
	{
		XrInstance instance = XR_NULL_HANDLE;
		const char *enabled[1];
		uint32_t    n_enabled = 0;

		struct { const char *label; XrVersion ver; } attempts[] = {
			{ "1.0", XR_API_VERSION_1_0 },
			{ "1.1", XR_API_VERSION_1_1 },
		};
		int a;

		if( have_gl ) enabled[n_enabled++] = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;

		for( a = 0; a < (int)( sizeof( attempts ) / sizeof( attempts[0] )); a++ )
		{
			XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };

			strcpy( ci.applicationInfo.applicationName, "XashPCVR probe" );
			strcpy( ci.applicationInfo.engineName, "Xash3D FWGS" );
			ci.applicationInfo.applicationVersion = 1;
			ci.applicationInfo.engineVersion = 1;
			ci.applicationInfo.apiVersion = attempts[a].ver;
			ci.enabledExtensionCount = n_enabled;
			ci.enabledExtensionNames = n_enabled ? enabled : NULL;

			res = xrCreateInstance( &ci, &instance );
			printf( "xrCreateInstance(apiVersion=%s) -> %s\n", attempts[a].label,
				result_str( XR_NULL_HANDLE, res ));
			if( XR_SUCCEEDED( res ))
			{
				printf( "  => USE apiVersion %s IN ENGINE CODE.\n", attempts[a].label );
				break;
			}
			instance = XR_NULL_HANDLE;
		}

		if( instance == XR_NULL_HANDLE )
		{
			printf( "\nAll xrCreateInstance attempts failed.\n" );
			printf( "  If the last error was -51 (RUNTIME_UNAVAILABLE), just start\n" );
			printf( "  Virtual Desktop Streamer / connect the headset and re-run.\n" );
			free( exts );
			return 2;
		}

		{
			XrInstanceProperties ip = { XR_TYPE_INSTANCE_PROPERTIES };
			if( XR_SUCCEEDED( xrGetInstanceProperties( instance, &ip )))
			{
				printf( "[runtime] name    : %s\n", ip.runtimeName );
				printf( "[runtime] version : %u.%u.%u\n",
					(unsigned)XR_VERSION_MAJOR( ip.runtimeVersion ),
					(unsigned)XR_VERSION_MINOR( ip.runtimeVersion ),
					(unsigned)XR_VERSION_PATCH( ip.runtimeVersion ));
			}
		}

		/* ---- 4. System + view configuration ---- */
		{
			XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
			XrSystemId      sysid = XR_NULL_SYSTEM_ID;

			sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
			res = xrGetSystem( instance, &sgi, &sysid );
			if( XR_FAILED( res ))
			{
				printf( "\nxrGetSystem FAILED: %s\n", result_str( instance, res ));
				printf( "  -> Runtime is installed but no HMD is currently available.\n" );
				printf( "  -> Extension list above is still valid and is the key result.\n" );
			}
			else
			{
				XrSystemProperties sp = { XR_TYPE_SYSTEM_PROPERTIES };

				if( XR_SUCCEEDED( xrGetSystemProperties( instance, sysid, &sp )))
				{
					printf( "\n[system] name        : %s\n", sp.systemName );
					printf( "[system] vendorId    : %u\n", sp.vendorId );
					printf( "[system] maxSwapchain: %ux%u, %u layers\n",
						sp.graphicsProperties.maxSwapchainImageWidth,
						sp.graphicsProperties.maxSwapchainImageHeight,
						sp.graphicsProperties.maxLayerCount );
					printf( "[system] orientation : %s   position: %s\n",
						sp.trackingProperties.orientationTracking ? "yes" : "no",
						sp.trackingProperties.positionTracking ? "yes" : "no" );
				}

				count = 0;
				if( XR_SUCCEEDED( xrEnumerateViewConfigurationViews( instance, sysid,
					XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, NULL )) && count )
				{
					XrViewConfigurationView *views =
						(XrViewConfigurationView *)calloc( count, sizeof( XrViewConfigurationView ));

					for( i = 0; i < count; i++ )
						views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;

					if( XR_SUCCEEDED( xrEnumerateViewConfigurationViews( instance, sysid,
						XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, views )))
					{
						printf( "\n[stereo views] %u\n", count );
						for( i = 0; i < count; i++ )
						{
							printf( "    eye %u: recommended %ux%u (max %ux%u), samples %u\n", i,
								views[i].recommendedImageRectWidth,
								views[i].recommendedImageRectHeight,
								views[i].maxImageRectWidth,
								views[i].maxImageRectHeight,
								views[i].recommendedSwapchainSampleCount );
						}
					}
					free( views );
				}

				/* GL graphics requirements - proves the GL path is really wired up */
				if( have_gl )
				{
					PFN_xrGetOpenGLGraphicsRequirementsKHR pfn = NULL;

					if( XR_SUCCEEDED( xrGetInstanceProcAddr( instance,
						"xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&pfn )) && pfn )
					{
						XrGraphicsRequirementsOpenGLKHR gr = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };

						res = pfn( instance, sysid, &gr );
						if( XR_SUCCEEDED( res ))
						{
							printf( "\n[GL requirements] min OpenGL %u.%u  max %u.%u\n",
								(unsigned)XR_VERSION_MAJOR( gr.minApiVersionSupported ),
								(unsigned)XR_VERSION_MINOR( gr.minApiVersionSupported ),
								(unsigned)XR_VERSION_MAJOR( gr.maxApiVersionSupported ),
								(unsigned)XR_VERSION_MINOR( gr.maxApiVersionSupported ));
							printf( "  => GL binding is fully functional on this runtime.\n" );
						}
						else printf( "\n[GL requirements] query FAILED: %s\n", result_str( instance, res ));
					}
					else printf( "\n[GL requirements] xrGetOpenGLGraphicsRequirementsKHR not resolvable\n" );
				}
			}
		}

		xrDestroyInstance( instance );
	}

	free( exts );
	printf( "\nprobe done.\n" );
	return 0;
}
