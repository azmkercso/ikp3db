IKPdb Integration Guide
=======================

The idea behind integration is to configure the debugger to automatically opens 
a debugger client with all execution information each time an error occurs.

By default, IKPdb will open a debugger client each time an unmanaged exception 
is raised during program execution.

Integration will allows to do the same with programs or frameworks which 
includes their own exception manager.

Example
-------

The `Odoo software <https://www.odoo.com>`_ integrates it's own exception manager.
As shown on the next picture, in case of error, Odoo rolls back the current 
transaction, opens a window displaying a stacktrace and resume execution 
when you click the "Ok" button.

.. image:: intguide_odoo_error_std.png

Once integrated, when an error occurs, you automatically get this before 
the Odoo stack trace.

.. image:: intguide_odoo_error_int.png

This saves you the time to open the file, find the line, set a breakpoint 
and restart. With integration you can start inspect variables and 
fix your bug straight.

.. _integration-api:

Integration API
---------------

You can use 2 functions to integrate IKPDb in your code:

* post_mortem()
* set_trace()

.. autofunction:: ikpdb.post_mortem

.. autofunction:: ikpdb.set_trace


Example implementations
-----------------------

This list contains instructions or pointers to material describing how to 
integrate some system or frameworks with IKPdb.

`Odoo <httsp://odoo.com>`_ integration
______________________________________

For Odoo 10
###########

You must modify odoo/openerp/tools/debugger.py to add **ikpdb** in the 
SUPPORTED_DEBUGGER list like this:

.. code-block:: python

    # -*- coding: utf-8 -*-
    # Part of Odoo. See LICENSE file for full copyright and licensing details.
    import importlib
    import logging
    import types
    
    _logger = logging.getLogger(__name__)
    SUPPORTED_DEBUGGER = {'pdb', 'ipdb', 'wdb', 'pudb', 'ikpdb'}


For Odoo 8 and 9
################

You must modify odoo/openerp/tools/debugger.py by inserting the lines surrended 
by the *IKPdb integration comment*.

.. code-block:: python

    # -*- coding: utf-8 -*-
    # Copyright: 2014 - OpenERP S.A. <http://openerp.com>
    import types
    
    def post_mortem(config, info):
        if config['debug_mode'] and isinstance(info[2], types.TracebackType):
            try:
                import pudb as pdb
            except ImportError:
                try:
                    import ipdb as pdb
                except ImportError:
                    ###( IKPdb integration begin )###
                    try:
                        import ikpdb ; ikpdb.post_mortem(info)
                        return
                    except:
                        pass
                    ###( IKPdb integration end )###
                    import pdb
            pdb.post_mortem(info[2])

For Odoo 7
##########

You must make the same modification in openerp/openerp/netsvc.py line 335.

Inouk Job Engine for Odoo
-------------------------

The "Inouk Job Engine" job queue management system for Odoo 9 is integrated with
IKPdb .
In the "Debug / Log" tab a flag allows to open IKPdb in port mortem mode 
if a non managed exception is raised by a job.



