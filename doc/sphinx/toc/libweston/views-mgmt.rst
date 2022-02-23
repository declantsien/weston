Views, surfaces and paint nodes
===============================

libweston presents to its users a few structure entities designed to abstract
some of the lower levels interactions which the back-ends or the renderers need
to work with.  Specifically these are:

- :type:`weston_surface`
- :type:`weston_view`
- :type:`weston_paint_node`

A while back, these first two entities used be together but now they're currently
separated, with the paint node being added much more later.

Surfaces and views
^^^^^^^^^^^^^^^^^^

A :type:`weston_surface` structure stores everything required for a client-side
or server-side surface.  This includes information about buffers, callbacks,
private data, input, damage, and regions, and other bookkeeping information.

A :type:`weston_view` structure represents an entity in the scene graph and
stores all of the geometry information. Includes some various regions (clip),
alpha, position, the transformation list as well as all of the
temporary information derived from the geometry state.

As a :type:`weston_view` is a direct child of a surface it is destroyed when
the surface is destroyed as well. Surfaces and views both keep track of which
outputs they are on.

The compositor maintains a `view_list` in :type:`weston_compositor` entity,
which is being being built as a tree, from layers (described a bit below), from 
such that back-ends can remain completely surface-/sub-surface agnostic, which 
happens upon an output repaint, or when a view has been destroyed.

Paint nodes
-----------

As a part of preparing for an output repaint, a :type:`weston_paint_node`
object is being created dynamically, which applies only to surfaces that are
going through an output repaint.  For that happen, surfaces are mapped, which
means they also must have mapped view.  As these paint nodes come into
existence, they're being added to the output's z-ordered paint node list.
Further on, using this output paint node list, they're being used/consumed by the
back-ends and the renderers.

.. note::

        With the exception of input regions, not going through a repaint, view
        assignment to a plane, or the paint node not coming into existence, all
        other places now make use of a :type:`weston_paint_node` z-ordered
        list.

View and surface creation
^^^^^^^^^^^^^^^^^^^^^^^^^

libweston users can create views by using :func:`weston_view_create` together
with :func:`weston_surface_create`, to create a surface.

.. code-block:: c

        struct weston_surface *surface;
        struct weston_view *view_a;
        struct weston_view *view_b;

        surface = weston_surface_create(compositor);
        view_a = weston_view_create(surface);
        view_b = weston_view_create(surface);
        /* ... */
        /* destroying the surface takes care of destroying the views */
        weston_surface_unref(surface);

:type:`weston_surface` structure has a reference counting in place, which
is under two forms:

- a weak reference, handled by a destroy listener, which being installed upon
  together when binding for `wl_compositor <https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_compositor>`_
  whose interface has a job of creating and destroying surfaces
- a strong reference, from which other users can use :func:`weston_surface_ref`
  to take another reference for it

Once a client disconnects, the :type:`weston_surface` destruction is being
automatically handled, but in case one might want to have the surface survive
that destruction, you'd need to take another reference to it. For instance,
when using libwston-desktop, the following could be used to obtain a new
ownership of the surface:

.. code-block:: c

        struct weston_desktop_surface *dsurface;
        struct weston_surface *wsurface =
                weston_desktop_surface_get_surface(dsurface);

        /* take another reference and create a view */
        struct weston_surface *wsurface_ref = weston_surface_ref(wsurface);
        struct weston_view *view = weston_view_create(wsurface_ref);

With `wsurface_ref` being taken, and with the client disconnecting, the view
and the surface will still be kept, until explicitly releasing using
:func:`weston_surface_unref()`.

Layers and surface damage
^^^^^^^^^^^^^^^^^^^^^^^^^

As a view, and not a surface, is a scene graph element, the view is what is
placed in layers, and in planes. Layers are used to stash views, a place to
add/move/remove views dynamically. Layers are stacked based on
their position by using :func:`weston_layer_set_position`. libweston contains
values in :func:`weston_layer_set_position` enum for the layer stacking order.

.. note::

        libweston can create adjust the layer's positioned, but simply
        adjusting the values found :type:`weston_layer_position` enum. And
        increase will basically move up the layers in stack, with a decrease
        making below the one supplied.

Once a layer has been created, setting it up has to be done using
:func:`weston_layer_init` which will be adding that layer into a layer list
present the :type:`weston_compositor` instance.

By traversing over this layer list, in the :type:`weston_compositor` instance,
as well as going over the views in each layer, a compositor's view list is
created, moment in which :type:`weston_paint_nodes` are also brought into
existence.

The following examples illustrates how to create, and add a view to a layer.
Further more, in order to tell the back-end to update a possibly repaint
we need to propagate any surface damage being inflicted to it.

.. code-block:: c

        struct weston_layer normal_layer;
        struct weston_view *view_already_created;

        /* create and set a positon for a layer */
        weston_layer_init(&normal_layer, weston_compositor);
        weston_layer_set_position(&normal_layer, WESTON_LAYER_POSITION_NORMAL);

        /* add the view to the layer and inflict damage on its surface */
        weston_layer_entry_insert(&normal_layer, &view_already_created->layer_link);
        weston_view_geometry_dirty(view_already_created);
        weston_surface_damage(view_already_created->surface);

Views created by libweston-desktop
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

libweston-desktop has been extensively described in :ref:`libweston-desktop`,
and here we're going to focus here just on the view/surface creation part.
libweston-desktop API provides functions to create views, using
:func:`weston_desktop_surface_create_view`:

.. code-block:: c

        struct weston_desktop_surface *dsurface;
        struct weston_view *view;

        /* view creation */
        view = weston_desktop_surface_create_view(dsurface);

        /* view unlinking and destruction */
        weston_desktop_surface_unlink_view(view);
        /* after that we can destroy the view */
        weston_view_destroy(view);

Once we've finished out with the view, we need to tell libweston-desktop we're
no longer using the view by calling :func:`weston_desktop_surface_unlink_view`.
Only after that we're safe destroying the :type:`weston_view`, by using
:func:`weston_view_destroy`. Attempting to destroy the view before severing the
link between :type:`weston_view` and an internal representation being used in
libweston-desktop is illegal.
