/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * Cockpit is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * Cockpit is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Cockpit; If not, see <https://www.gnu.org/licenses/>.
 */

import cockpit from "cockpit";

const _ = cockpit.gettext;

export const client = {};

const busName = "org.fedoraproject.Setroubleshootd";
const dbusInterface = "org.fedoraproject.SetroubleshootdIface";
const dbusPath = "/org/fedoraproject/Setroubleshootd";

const busNameFixit = "org.fedoraproject.SetroubleshootFixit";
const dbusInterfaceFixit = busNameFixit;
const dbusPathFixit = "/org/fedoraproject/SetroubleshootFixit/object";

client.init = function() {
    client.connected = false;
    const dbusClientSeTroubleshoot = cockpit.dbus(busName, { superuser: "try" });
    client.proxy = dbusClientSeTroubleshoot.proxy(dbusInterface, dbusPath);

    client.proxyFixit = cockpit.dbus(busNameFixit, { superuser: "try" }).proxy(dbusInterfaceFixit, dbusPathFixit);

    const connectPromise = new Promise((resolve, reject) => {
        client.proxy.wait(() => {
            // HACK setroubleshootd seems to drop connections if we don't start explicitly
            client.proxy.call("start", [])
                    .then(() => {
                        client.connected = true;
                        resolve();
                    })
                    .catch(ex => reject(new Error(_("Unable to start setroubleshootd"))));
        });
    });

    client.alertCallback = null;

    function handleSignal(event, name, args) {
        if (client.alertCallback && name == "alert") {
            const level = args[0];
            const localId = args[1];
            client.alertCallback(level, localId);
        }
    }

    // register to receive calls whenever a new alert becomes available
    // signature for the alert callback: (level, localId)
    client.handleAlert = function(callback) {
        // if we didn't listen to events before, do so now
        if (!client.alertCallback) {
            client.proxy.addEventListener("signal", handleSignal);
        }
        client.alertCallback = callback;
    };

    // returns a promise
    client.getAlerts = function(since) {
        return new Promise((resolve, reject) => {
            let call;
            if (since !== undefined)
                call = client.proxy.call("get_all_alerts_since", [since]);
            else
                call = client.proxy.call("get_all_alerts", []);
            call
                    .then(result => {
                        resolve(result[0].map(entry => ({
                            localId: entry[0],
                            summary: entry[1],
                            reportCount: entry[2],
                        })));
                    })
                    .catch(ex => reject(ex));
        });
    };

    /* Return an alert with summary, audit events, fix suggestions (by id)
      localId: an alert id
      summary: a brief description of an alert. E.g.
                  "SELinux is preventing /usr/bin/bash from ioctl access on the unix_stream_socket unix_stream_socket."
      reportCount: count of reports of this alert
      auditEvent: an array of audit events (AVC, SYSCALL) connected to the alert
      pluginAnalysis: an array of plugin analysis structure
          ifText
          thenText
          doText
          analysisId: plugin id. It can be used in org.fedoraproject.SetroubleshootFixit.run_fix()
          fixable: True when an alert is fixable by a plugin
          reportBug: True when an alert should be reported
      firstSeen: when the alert was seen for the first time, timestamp in ms
      lastSeen: when the alert was seen for the last time, timestamp in ms
      level: "green", "yellow" or "red"
    */
    client.getAlert = localId => client.proxy.call("get_alert", [localId])
            .then(result => {
                const details = {
                    localId: result[0],
                    summary: result[1],
                    reportCount: result[2],
                    auditEvent: result[3],
                    pluginAnalysis: result[4],
                    firstSeen: result[5] / 1000,
                    lastSeen: result[6] / 1000,
                    level: result[7],
                };
                // cleanup analysis
                details.pluginAnalysis = details.pluginAnalysis.map(itm => ({
                    ifText: itm[0],
                    thenText: itm[1],
                    doText: itm[2],
                    analysisId: itm[3],
                    fixable: itm[4],
                    reportBug: itm[5],
                }));
                return details;
            })
            .catch(ex => {
                console.warn("Unable to get alert for id", localId, ":", JSON.stringify(ex));
                return Promise.reject(new Error(`Unable to get alert for id ${localId}: ${ex.toString()}`));
            });

    /* Run a fix via SetroubleshootFixit
       The analysisId is given as part of pluginAnalysis entries in alert details
     */
    client.runFix = (alertId, analysisId) => client.proxyFixit.call("run_fix", [alertId, analysisId])
            .then(result => result[0])
            .catch(ex => {
                console.warn("Unable to run fix:", JSON.stringify(ex));
                return Promise.reject(new Error(cockpit.format(_("Unable to run fix: $0"), ex.toString())));
            });

    /* Delete an alert from the database (will be removed for all users), returns true on success
     * Only assign this to the client variable if the dbus interface actually supports the operation
     */
    client.deleteAlert = localId => client.proxy.call("delete_alert", [localId])
            .then(success => {
                if (!success)
                    return Promise.reject(new Error(cockpit.format(_("Failed to delete alert: $0"), localId)));
            })
            .catch(ex => {
                console.warn("Unable to delete alert with id", localId, ":", JSON.stringify(ex));
                return Promise.reject(new Error(cockpit.format(_("Failed to delete alert: $0"), ex.toString())));
            });

    // connect to dbus and start setroubleshootd
    return connectPromise;
};
