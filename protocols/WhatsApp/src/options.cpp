/*

WhatsApp plugin for Miranda NG
Copyright � 2019-25 George Hazan

*/

#include "stdafx.h"

/////////////////////////////////////////////////////////////////////////////////////////

class COptionsDlg : public CProtoDlgBase<WhatsAppProto>
{
	CCtrlCheck chkHideChats, chkBbcodes;
	CCtrlEdit edtGroup, edtNick, edtDevName;
	CCtrlButton btnUnregister;
	ptrW m_wszOldGroup;

public:
	COptionsDlg(WhatsAppProto *ppro, int iDlgID, bool bFullDlg) :
		CProtoDlgBase<WhatsAppProto>(ppro, iDlgID),
		chkBbcodes(this, IDC_USEBBCODES),
		chkHideChats(this, IDC_HIDECHATS),
		edtNick(this, IDC_NICK),
		edtGroup(this, IDC_DEFGROUP),
		edtDevName(this, IDC_DEVICE_NAME),
		btnUnregister(this, IDC_UNREGISTER),
		m_wszOldGroup(mir_wstrdup(ppro->m_wszDefaultGroup))
	{
		CreateLink(edtNick, ppro->m_wszNick);
		CreateLink(edtGroup, ppro->m_wszDefaultGroup);
		CreateLink(edtDevName, ppro->m_wszDeviceName);
		CreateLink(chkHideChats, ppro->m_bHideGroupchats);

		if (bFullDlg)
			CreateLink(chkBbcodes, ppro->m_bUseBbcodes);

		btnUnregister.OnClick = Callback(this, &COptionsDlg::onClick_Unregister);
	}

	bool OnInitDialog() override
	{
		if (!m_proto->getMStringA(DBKEY_ID).IsEmpty())
			edtDevName.Disable();
		return true;
	}

	bool OnApply() override
	{
		if (mir_wstrlen(m_proto->m_wszNick)) {
			SetFocus(edtNick.GetHwnd());
			return false;
		}

		if (mir_wstrcmp(m_proto->m_wszDefaultGroup, m_wszOldGroup))
			Clist_GroupCreate(0, m_proto->m_wszDefaultGroup);
		return true;
	}

	void onClick_Unregister(CCtrlButton *)
	{
		if (IDYES != MessageBoxW(0, TranslateT("Do you really want to log out from account? You will have to link your devices again."), m_proto->m_tszUserName, MB_ICONQUESTION | MB_YESNO))
			return;

		if (m_proto->isOnline())
			m_proto->SendUnregister();
		else
			m_proto->OnErase();
	}
};

/////////////////////////////////////////////////////////////////////////////////////////

MWindow WhatsAppProto::OnCreateAccMgrUI(MWindow hwndParent)
{
	auto *pDlg = new COptionsDlg(this, IDD_ACCMGRUI, false);
	pDlg->SetParent(hwndParent);
	pDlg->Create();
	return pDlg->GetHwnd();
}

int WhatsAppProto::OnOptionsInit(WPARAM wParam, LPARAM)
{
	OPTIONSDIALOGPAGE odp = {};
	odp.szTitle.w = m_tszUserName;
	odp.flags = ODPF_UNICODE;
	odp.szGroup.w = LPGENW("Network");

	odp.position = 1;
	odp.szTab.w = LPGENW("Account");
	odp.pDialog = new COptionsDlg(this, IDD_OPTIONS, true);
	g_plugin.addOptions(wParam, &odp);
	return 0;
}
