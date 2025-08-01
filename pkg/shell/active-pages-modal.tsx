/*
 * This file is part of Cockpit.
 *
 * Copyright (C) 2021 Red Hat, Inc.
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

import React, { useState } from "react";
import { ListingTable } from "cockpit-components-table.jsx";
import { Button } from "@patternfly/react-core/dist/esm/components/Button/index.js";
import { Label } from "@patternfly/react-core/dist/esm/components/Label/index.js";
import { Split, SplitItem } from "@patternfly/react-core/dist/esm/layouts/Split/index.js";
import {
    Modal,
    ModalBody,
    ModalFooter,
    ModalHeader
} from '@patternfly/react-core/dist/esm/components/Modal/index.js';
import { useInit } from "hooks";
import { DialogResult } from "dialogs";

import { ShellState } from "./state";

const _ = cockpit.gettext;

export interface ActivePagesDialogProps {
    dialogResult: DialogResult<void>;
    state: ShellState;
}

export const ActivePagesDialog = ({
    dialogResult,
    state
}: ActivePagesDialogProps) => {
    function get_pages() {
        const result = [];
        for (const frame of Object.values(state.frames)) {
            if (frame.url) {
                const active = (frame == state.current_frame ||
                                state.most_recent_path_for_host(frame.host) == frame.path);
                result.push({
                    frame,
                    active,
                    selected: active,
                    displayName: frame.host === "localhost" ? "/" + frame.path : frame.host + ":/" + frame.path,
                });
            }
        }

        // sort the frames by displayName, active ones first
        result.sort(function(a, b) {
            return (a.active ? -2 : 0) + (b.active ? 2 : 0) +
                   ((a.displayName < b.displayName) ? -1 : 0) + ((b.displayName < a.displayName) ? 1 : 0);
        });

        return result;
    }

    const init_pages = useInit(get_pages, [frames]);
    const [pages, setPages] = useState(init_pages);

    function onRemove() {
        pages.forEach(element => {
            if (element.selected) {
                state.remove_frame(element.frame.name);
            }
        });
        dialogResult.resolve();
    }

    const rows = pages.map(page => {
        const columns = [{
            title: <Split>
                <SplitItem isFilled>
                    {page.displayName}
                </SplitItem>
                <SplitItem>
                    {page.active && <Label color="blue">{_("active")}</Label>}
                </SplitItem>
            </Split>,
        }];
        return ({
            props: {
                key: page.frame.name,
                'data-row-id': page.frame.name
            },
            columns,
            selected: page.selected,
        });
    });

    return (
        <Modal isOpen position="top" variant="small"
               id="active-pages-dialog"
               onClose={() => dialogResult.resolve()}
        >
            <ModalHeader title={_("Active pages")} />
            <ModalBody>
                <ListingTable showHeader={false}
                              columns={[{ title: _("Page name") }]}
                              aria-label={_("Active pages")}
                              emptyCaption={ _("There are currently no active pages") }
                                  onSelect={(_event, isSelected, rowIndex) => {
                                      const new_pages = [...pages];
                                      new_pages[rowIndex].selected = isSelected;
                                      setPages(new_pages);
                                  }}
                              rows={rows} />
            </ModalBody>
            <ModalFooter>
                <Button variant='primary' onClick={onRemove}>{_("Close selected pages")}</Button>
                <Button variant='link' onClick={() => dialogResult.resolve()}>{_("Cancel")}</Button>
            </ModalFooter>
        </Modal>
    );
};
