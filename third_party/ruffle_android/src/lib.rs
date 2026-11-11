// The iOS port's Ruffle bridge with its surfaces made over an ANativeWindow.
// The exported names stay ruffle_ios_*, the app code is shared with that port.

#![allow(clippy::missing_safety_doc)]

use std::ffi::c_void;
use std::ptr::NonNull;

use raw_window_handle::{
    AndroidDisplayHandle, AndroidNdkWindowHandle, RawDisplayHandle, RawWindowHandle,
};

pub(crate) const BACKENDS: wgpu::Backends = wgpu::Backends::VULKAN;

// The pointer the app hands over as a layer is an ANativeWindow
pub(crate) unsafe fn window_surface_target(window: *mut c_void) -> wgpu::SurfaceTargetUnsafe {
    let window = NonNull::new(window).expect("null ANativeWindow");
    wgpu::SurfaceTargetUnsafe::RawHandle {
        raw_display_handle: RawDisplayHandle::Android(AndroidDisplayHandle::new()),
        raw_window_handle: RawWindowHandle::AndroidNdk(AndroidNdkWindowHandle::new(window)),
    }
}

include!(concat!(env!("OUT_DIR"), "/bridge.rs"));

// A surface goes with its window when the app is put in the background.
// These put the player's two surfaces over the windows that come back.

#[no_mangle]
pub unsafe extern "C" fn ruffle_android_player_set_window(
    h: *mut PlayerHandle,
    window: *mut c_void,
    width: c_int,
    height: c_int,
) -> c_int {
    let Some(handle) = h.as_ref() else { return 0; };
    if window.is_null() {
        return 0;
    }
    let Ok(mut p) = handle.player.lock() else { return 0; };
    let renderer: &mut dyn std::any::Any = p.renderer_mut();
    let Some(backend) = renderer.downcast_mut::<OverlayBackend>() else { return 0; };
    let size = (width.max(1) as u32, height.max(1) as u32);
    match backend.recreate_surface_unsafe(window_surface_target(window), size) {
        Ok(()) => 1,
        Err(e) => {
            eprintln!("[ruffle_android] recreate_surface_unsafe: {e:?}");
            0
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn ruffle_android_overlay_set_window(
    h: *mut PlayerHandle,
    window: *mut c_void,
    width: c_int,
    height: c_int,
) -> c_int {
    let Some(handle) = h.as_ref() else { return 0; };
    if window.is_null() {
        return 0;
    }
    let mut overlay = handle.overlay.borrow_mut();
    let Some(backend) = overlay.as_mut() else { return 0; };
    let size = (width.max(1) as u32, height.max(1) as u32);
    match backend.recreate_surface_unsafe(window_surface_target(window), size) {
        Ok(()) => {
            handle.overlay_dirty.set(true);
            1
        }
        Err(e) => {
            eprintln!("[ruffle_android] overlay recreate_surface_unsafe: {e:?}");
            0
        }
    }
}

// The windows kept and only their size changed
#[no_mangle]
pub unsafe extern "C" fn ruffle_android_player_resize(h: *mut PlayerHandle, width: c_int, height: c_int) {
    use ruffle_render::backend::{RenderBackend, ViewportDimensions};
    let Some(handle) = h.as_ref() else { return; };
    let dimensions = ViewportDimensions {
        width: width.max(1) as u32,
        height: height.max(1) as u32,
        scale_factor: 1.0,
    };
    if let Ok(mut p) = handle.player.lock() {
        p.set_viewport_dimensions(dimensions);
    }
    if let Some(backend) = handle.overlay.borrow_mut().as_mut() {
        backend.set_viewport_dimensions(dimensions);
        handle.overlay_dirty.set(true);
    }
}
